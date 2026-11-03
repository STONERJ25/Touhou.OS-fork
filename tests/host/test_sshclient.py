"""Host tests of the ssh client (docs/SSH_CLIENT.md): the library's pure pieces against OpenSSH and independent implementations, then the client programs
themselves, built for the host, against a small SSH server written for the purpose (tests/sshsrv.py).

The sources under test are the very files the OS is built from (user/lib/sshcrypto/src/sshc_*.c, user/bin/ssh*/...); only sshc_random() and the include
paths differ (-DSSHC_HOST), and they are built with AddressSanitizer and UBSan. Expected values come from places that share no code with them: Python's hashlib
and hmac, the `cryptography` package, the `bcrypt` package's kdf(), and the real OpenSSH tools (ssh-keygen, ssh-keyscan) of the host.

    python tools/hosttest.py discover -s tests/host -p "test_sshclient.py"           (on Windows this runs inside WSL's Ubuntu)
"""
import base64
import hashlib
import hmac
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests"))
LIB = ROOT / "user" / "lib" / "sshcrypto"
OUT = ROOT / "build" / "host-ssh"

KEYGEN = shutil.which("ssh-keygen")
KEYSCAN = shutil.which("ssh-keyscan")


def have_gcc():
    return shutil.which("gcc") is not None


def lib_sources():
    """Everything the OS links into libsshcrypto.a, plus libgap's crypto.c for SHA-256 and MD5."""
    return sorted((LIB / "src").glob("*.c")) + [ROOT / "user" / "libc" / "src" / "crypto.c"]


def newest(paths):
    return max(p.stat().st_mtime for p in paths)


def build(name, sources, extra=(), sanitize=True, defines=()):
    """Builds a host program from C sources (gcc, -Wall -Wextra, ASan+UBSan). Returns its path; rebuilds only when a source or header is newer."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / name
    shim = OUT / "shim"
    shim.mkdir(exist_ok=True)
    (shim / "gensokyo.h").write_text("#ifndef SHIM_GENSOKYO_H\n#define SHIM_GENSOKYO_H\nint gap_getrandom(void *buf, unsigned long n);\n#endif\n")
    (shim / "gap_shim.c").write_text("#include <sys/random.h>\nint gap_getrandom(void *buf, unsigned long n) { return (int)getrandom(buf, n, 0); }\n")     # what libgap's crypto.c asks of the OS
    deps =list(sources) + list(LIB.glob("include/*.h")) + list(LIB.glob("src/*.h")) + [Path(__file__)]
    for d in list((ROOT / "user" / "bin").glob("ssh*")) + [ROOT / "user" / "bin" / "scp", ROOT / "user" / "bin" / "sftp"]:
        deps += list(d.glob("*.[ch]"))
    if exe.exists() and exe.stat().st_mtime >= newest([p for p in deps if p.exists()]):
        return exe
    flags = ["-std=gnu17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
             "-DSSHC_HOST=1", *[f"-D{d}" for d in defines]]
    if sanitize:
        # shift-base is off because the existing field arithmetic (curve25519.c, TweetNaCl's idiom) shifts negative numbers left, which every compiler
        # treats as two's complement but which UBSan reports; that file is not part of the client and is covered by the OS's own sshcrypto_known_answers
        flags += ["-fsanitize=address,undefined", "-fno-sanitize=shift-base", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
    cmd = ["gcc", *flags, "-I", str(LIB / "include"), "-I", str(shim), "-idirafter", str(ROOT / "user" / "libc" / "include"), "-I", str(ROOT / "shared"),
           *[f"-I{p}" for p in extra if Path(p).is_dir()], "-o", str(exe), *map(str, sources), str(shim / "gap_shim.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or re.search(r"warning:", r.stderr):
        raise RuntimeError("building %s failed or warned:\n%s" % (name, r.stderr[-6000:]))
    return exe


_programs = {}


def program(name):
    """The client program `name` (ssh, ssh-keygen, ssh-keyscan, scp, sftp) built for the host from user/bin/<name>/*.c and the library."""
    if name not in _programs:
        src = sorted((ROOT / "user" / "bin" / name).glob("*.c"))
        _programs[name] = build("host-" + name, src + lib_sources(), extra=[ROOT / "user" / "bin" / name])
    return _programs[name]


_tool = None


def tool(*args, check=True, input=None):
    """Runs tests/host/sshclient_tool.c with the arguments; returns its standard output (text)."""
    global _tool
    if _tool is None:
        _tool = build("sshclient_tool", [HERE / "sshclient_tool.c", *lib_sources()])
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([str(_tool), *map(str, args)], capture_output=True, text=True, env=env, input=input)
    if check and r.returncode != 0:
        raise AssertionError("sshclient_tool %s failed (%d):\n%s%s" % (" ".join(map(str, args)), r.returncode, r.stdout, r.stderr))
    return r.stdout


def keygen(*args, check=True):
    r = subprocess.run([KEYGEN, *map(str, args)], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise AssertionError("ssh-keygen %s failed: %s" % (" ".join(map(str, args)), r.stderr))
    return r.stdout


def pi_hex_words(count):
    """The first `count` 32-bit words of the fraction of pi (Machin's formula in integers): Blowfish's initial P-array and S-boxes."""
    def arctan_inv(x, bits):
        one = 1 << bits
        total = term = one // x
        x2 = x * x
        n, sign = 1, -1
        while term:
            term //= x2
            n += 2
            total += sign * (term // n)
            sign = -sign
        return total
    bits = count * 32 + 64
    pi = 16 * arctan_inv(5, bits) - 4 * arctan_inv(239, bits)
    frac = (pi - (3 << bits)) >> 64
    return [(frac >> (32 * (count - 1 - i))) & 0xFFFFFFFF for i in range(count)]


@unittest.skipUnless(have_gcc(), "needs gcc")
class Primitives(unittest.TestCase):
    def test_sha1_and_hmac(self):
        for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 1000):
            data = bytes((i * 7 + n) & 255 for i in range(n))
            self.assertEqual(tool("sha1", data.hex()).strip(), hashlib.sha1(data).hexdigest(), n)
        for klen in (1, 20, 64, 65, 200):
            key = bytes(range(klen % 256)) * 1 or b"k"
            key = (key * 300)[:klen]
            msg = b"the quick brown fox" * 3
            self.assertEqual(tool("hmac-sha1", key.hex(), msg.hex()).strip(), hmac.new(key, msg, "sha1").hexdigest(), klen)

    def test_blowfish_table_is_pi(self):
        words = pi_hex_words(1042)
        text = (LIB / "src" / "sshc_blowfish_tab.h").read_text()
        got = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})u", text)]
        self.assertEqual(got, words)
        self.assertEqual((got[0], got[18]), (0x243F6A88, 0xD1310BA6))                    # P[0] and S0[0], as in Schneier's paper

    def test_bcrypt_pbkdf_matches_the_bcrypt_package(self):
        import bcrypt
        cases = [(b"password", b"salt", 4, 32), (b"password", b"salt", 4, 48), (b"a passphrase with spaces", bytes(range(16)), 16, 48),
                 (b"x", os.urandom(16), 5, 64), (b"\xff\xfe\x00bytes", b"1234567890abcdef", 2, 17), (b"p" * 100, b"s" * 16, 3, 100)]
        for pw, salt, rounds, klen in cases:
            want = bcrypt.kdf(password=pw, salt=salt, desired_key_bytes=klen, rounds=rounds, ignore_few_rounds=True)
            self.assertEqual(tool("bcrypt", pw.hex(), salt.hex(), rounds, klen).strip(), want.hex(), (pw, rounds, klen))

    def test_aes_ctr_matches_cryptography(self):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        for klen in (16, 24, 32):
            for n in (1, 15, 16, 17, 100, 333):
                key, iv, data = os.urandom(klen), os.urandom(16), os.urandom(n)
                if n == 100:
                    iv = b"\xff" * 16                                    # the counter carries all the way round
                enc = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
                want = enc.update(data) + enc.finalize()
                self.assertEqual(tool("aesctr", key.hex(), iv.hex(), data.hex()).strip(), want.hex(), (klen, n))

    def test_base64_and_paths(self):
        self.assertEqual(tool("b64", "enc", "any carnal pleas").strip(), base64.b64encode(b"any carnal pleas").decode())
        for text in ("", "f", "fo", "foo", "foob", "fooba", "foobar"):
            enc = base64.b64encode(text.encode()).decode()
            self.assertEqual(tool("b64", "dec", enc).rstrip("\n"), text)
            self.assertEqual(tool("b64", "dec", enc.rstrip("=")).rstrip("\n"), text)     # padding is optional on input
        self.assertEqual(tool("b64", "dec", "ab=cd").strip(), "(invalid)")
        env = dict(os.environ, HOME="/home/someone")
        r = subprocess.run([str(build("sshclient_tool", [HERE / "sshclient_tool.c", *lib_sources()])), "expand", "~/.ssh/id_%h_%r_%p_%%", "ho.st", "bob", "2222"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.stdout.strip(), "/home/someone/.ssh/id_ho.st_bob_2222_%")
        self.assertIn("bad token", tool("expand", "%z", "h", "u", "22"))


@unittest.skipUnless(have_gcc() and KEYGEN, "needs gcc and ssh-keygen")
class PublicKeys(unittest.TestCase):
    """Fingerprints, key sizes and randomart are what ssh-keygen -l prints; known_hosts decisions follow ssh(1)."""

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix="sshc-pk-"))
        cls.keys = {}
        for name, args in (("a", ["-t", "ed25519"]), ("b", ["-t", "ed25519"]), ("rsa", ["-t", "rsa", "-b", "2048"]), ("ec", ["-t", "ecdsa", "-b", "256"]),
                           ("ec384", ["-t", "ecdsa", "-b", "384"])):
            path = cls.dir / name
            keygen("-q", *args, "-N", "", "-C", f"comment {name}", "-f", path)
            cls.keys[name] = (path, (cls.dir / (name + ".pub")).read_text().strip())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def pub(self, name):
        return str(self.keys[name][0]) + ".pub"

    def line(self, host, key):
        return host + " " + " ".join(self.keys[key][1].split()[:2])

    def test_fingerprints_and_labels_match_ssh_keygen(self):
        for name in self.keys:
            want = keygen("-l", "-f", self.pub(name)).strip()                                # "256 SHA256:... comment name (ED25519)"
            got = tool("fp", self.pub(name)).splitlines()[0]
            self.assertEqual(got, want, name)
            want_md5 = keygen("-l", "-E", "md5", "-f", self.pub(name)).split()[1]
            self.assertEqual(tool("fp", self.pub(name)).splitlines()[1], want_md5, name)

    def test_randomart_matches_ssh_keygen(self):
        for name in self.keys:
            out = keygen("-lv", "-f", self.pub(name))
            want = out.split("\n", 1)[1].rstrip("\n")                                        # the picture after the first line
            got = tool("fp", self.pub(name)).split("\n", 3)[3].rstrip("\n")                  # after: first line, md5, comment
            self.assertEqual(got, want, name)

    def check(self, text, host, port, key):
        f = self.dir / "kh_case"
        f.write_text(text)
        return tool("hkcheck", f, host, port, self.pub(key)).strip()

    def test_known_hosts_decisions(self):
        L = self.line
        cases = [
            (L("host1", "a"), "host1", 22, "a", "ok"), (L("host1", "a"), "host1", 22, "b", "changed"), (L("host1", "a"), "host2", 22, "a", "new"),
            (L("*.example.com,!bad.example.com", "a"), "good.example.com", 22, "a", "ok"),
            (L("*.example.com,!bad.example.com", "a"), "bad.example.com", 22, "a", "new"),
            (L("*.example.com,!bad.example.com", "a"), "example.com", 22, "a", "new"),
            (L("h?st1", "a"), "host1", 22, "a", "ok"), (L("h?st1", "a"), "hoost1", 22, "a", "new"),
            (L("[host1]:2222", "a"), "host1", 2222, "a", "ok"), (L("[host1]:2222", "a"), "host1", 22, "a", "new"),
            (L("host1", "a"), "host1", 2222, "a", "new"),
            (L("Host1", "a"), "HOST1", 22, "a", "ok"),
            (L("host3", "rsa"), "host3", 22, "a", "new other=1"),
            (L("@revoked *", "a"), "anything", 22, "a", "revoked"), (L("@revoked *", "a"), "anything", 22, "b", "new"),
            (L("@cert-authority host1", "a"), "host1", 22, "a", "new"),
            (L("host1", "rsa") + "\n" + L("host1", "a"), "host1", 22, "a", "ok"),
            ("# a comment\n\n   \n" + L("host1", "a") + "\r\n", "host1", 22, "a", "ok"),
            ("garbage line\nhost1 ssh-ed25519\nhost1 ssh-ed25519 !!!!\n" + L("host1", "a"), "host1", 22, "a", "ok"),
            (L("host1,host2,192.168.1.7", "a"), "192.168.1.7", 22, "a", "ok"),
            (L("*", "a"), "whatever", 22, "a", "ok"),
        ]
        for text, host, port, key, want in cases:
            got = self.check(text, host, port, key)
            self.assertTrue(got.startswith(want.split()[0]), (text, host, port, key, want, got))
            if "other=1" in want:
                self.assertIn("other=1", got)

    def test_known_hosts_line_numbers(self):
        text = "# c\n" + self.line("other", "b") + "\n" + self.line("host1", "b") + "\n" + self.line("host1", "a") + "\n"
        f = self.dir / "kh_lines"
        f.write_text(text)
        self.assertRegex(tool("hkcheck", f, "host1", 22, self.pub("a")), r"^ok .*kh_lines:4 ")
        self.assertRegex(tool("hkcheck", f, "host1", 22, self.pub("rsa")), r"^new ")
        f.write_text("# c\n" + self.line("other", "b") + "\n" + self.line("host1", "b") + "\n")
        self.assertRegex(tool("hkcheck", f, "host1", 22, self.pub("a")), r"^changed .*kh_lines:3 ")

    def test_hashed_entries_made_by_openssh(self):
        f = self.dir / "kh_hashed"
        f.write_text(self.line("host1,host2", "a") + "\n" + self.line("[host3]:2200", "b") + "\n" + self.line("*.wild", "a") + "\n")
        keygen("-H", "-f", f)
        text = f.read_text()
        self.assertEqual(text.count("|1|"), 3)                                              # host1, host2, [host3]:2200 hashed; the wildcard is left alone
        for host, port, key, want in (("host1", 22, "a", "ok"), ("host2", 22, "a", "ok"), ("host2", 22, "b", "changed"), ("host3", 2200, "b", "ok"),
                                      ("host3", 22, "b", "new"), ("x.wild", 22, "a", "ok"), ("host4", 22, "a", "new")):
            self.assertTrue(tool("hkcheck", f, host, port, self.pub(key)).startswith(want), (host, port, key))

    def test_added_entries_are_found_by_ssh_keygen(self):
        f = self.dir / "kh_added" / "sub" / "known_hosts"                                  # the directories are made on the way
        for host, port, hashed in (("plain.host", 22, 0), ("hashed.host", 22, 1), ("other.host", 2022, 0), ("hport.host", 2022, 1)):
            self.assertEqual(tool("hkadd", f, host, port, hashed, self.pub("a")).strip(), "0")
        for host, port in (("plain.host", 22), ("hashed.host", 22), ("other.host", 2022), ("hport.host", 2022)):
            name = host if port == 22 else f"[{host}]:{port}"
            self.assertIn(self.keys["a"][1].split()[1], keygen("-F", name, "-f", f), name)
            self.assertTrue(tool("hkcheck", f, host, port, self.pub("a")).startswith("ok"), host)
        self.assertEqual(oct(f.stat().st_mode & 0o777), "0o600")
        self.assertEqual(oct(f.parent.stat().st_mode & 0o777), "0o700")
        f.write_bytes(f.read_bytes().rstrip(b"\n"))                                         # no final newline: the next entry must not be glued to the last
        tool("hkadd", f, "last.host", 22, 0, self.pub("b"))
        self.assertTrue(tool("hkcheck", f, "last.host", 22, self.pub("b")).startswith("ok"))
        self.assertTrue(tool("hkcheck", f, "other.host", 2022, self.pub("a")).startswith("ok"))

    def test_patterns(self):
        for pat, s, want in (("*", "", "yes"), ("a*b", "axxb", "yes"), ("a*b", "axxbc", "no"), ("?", "a", "yes"), ("?", "", "no"), ("*.x", "a.x", "yes"),
                             ("A*", "abc", "yes"), ("a*a*a", "aaa", "yes"), ("a*a*a", "aa", "no"), ("**", "x", "yes"), ("", "", "yes"), ("", "x", "no")):
            self.assertEqual(tool("pattern", pat, s).strip(), want, (pat, s))


@unittest.skipUnless(have_gcc() and KEYGEN, "needs gcc and ssh-keygen")
class KeyFiles(unittest.TestCase):
    """openssh-key-v1: files written by ssh-keygen (plain and with passphrases, four ciphers) are read, and files written here are read by ssh-keygen."""

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix="sshc-kf-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def seed_of(self, path, pw=None):
        from cryptography.hazmat.primitives import serialization
        key = serialization.load_ssh_private_key(Path(path).read_bytes(), password=pw)
        return key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption()).hex()

    def test_plain_key_from_ssh_keygen(self):
        path = self.dir / "plain"
        keygen("-q", "-t", "ed25519", "-N", "", "-C", "plain comment", "-f", path)
        info = tool("keyinfo", path)
        self.assertIn("cipher=none kdf=none", info)
        self.assertIn((self.dir / "plain.pub").read_text().split()[1], info)
        out = tool("keyunlock", path)
        self.assertIn("seed=" + self.seed_of(path), out)
        self.assertIn("comment=plain comment", out)
        self.assertIn("pk=" + base64.b64decode((self.dir / "plain.pub").read_text().split()[1])[-32:].hex(), out)

    def test_encrypted_keys_from_ssh_keygen(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        for cipher in ("aes256-ctr", "aes128-ctr", "aes192-ctr", "chacha20-poly1305@openssh.com"):
            for rounds in (16, 3):
                path = self.dir / f"enc-{cipher.split('@')[0]}-{rounds}"
                keygen("-q", "-t", "ed25519", "-N", "correct horse", "-Z", cipher, "-a", rounds, "-C", "secret comment", "-f", path)
                self.assertIn(f"cipher={cipher} kdf=bcrypt rounds={rounds}", tool("keyinfo", path))
                out = tool("keyunlock", path, "correct horse")
                self.assertIn("comment=secret comment", out, cipher)
                self.assertIn("pk=" + base64.b64decode((self.dir / (path.name + ".pub")).read_text().split()[1])[-32:].hex(), out, cipher)
                seed = bytes.fromhex(re.search(r"seed=([0-9a-f]{64})", out).group(1))
                self.assertEqual(ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex(),
                                 re.search(r"pk=([0-9a-f]{64})", out).group(1), cipher)            # the decrypted seed really is this key's
                self.assertRegex(tool("keyunlock", path, "wrong"), r"^locked\(-2\): incorrect passphrase", cipher)
                self.assertRegex(tool("keyunlock", path), r"^locked\(-2\)", cipher)

    def test_keys_written_here_are_read_by_ssh_keygen(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        seed = os.urandom(32)
        want_pk = ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        for pw, rounds in (("-", 16), ("hunter2 with space", 16), ("pw", 4)):
            text = tool("keymake", seed.hex(), "made here", pw, rounds)
            path = self.dir / f"made-{rounds}-{'plain' if pw == '-' else 'enc'}"
            path.write_text(text)
            os.chmod(path, 0o600)
            self.assertTrue(text.startswith("-----BEGIN OPENSSH PRIVATE KEY-----\n") and text.endswith("-----END OPENSSH PRIVATE KEY-----\n"))
            self.assertTrue(all(len(line) <= 70 for line in text.splitlines()[1:-1]))
            pub = keygen("-y", "-f", path, *([] if pw == "-" else ["-P", pw])).strip()
            self.assertEqual(base64.b64decode(pub.split()[1])[-32:], want_pk)
            self.assertEqual(self.seed_of(path, None if pw == "-" else pw.encode()), seed.hex())       # an independent reader agrees about the seed
            back = tool("keyunlock", path, *([] if pw == "-" else [pw]))
            self.assertIn("seed=" + seed.hex(), back)
            self.assertIn("comment=made here", back)
            if pw != "-":
                self.assertIn("locked(-2)", tool("keyunlock", path, "nope"))
        path = self.dir / "made-16-enc"                                                           # the passphrase changes with ssh-keygen -p: still readable here
        keygen("-p", "-f", path, "-P", "hunter2 with space", "-N", "a new one")
        self.assertIn("seed=" + seed.hex(), tool("keyunlock", path, "a new one"))

    def test_damaged_and_foreign_files(self):
        good = tool("keymake", os.urandom(32).hex(), "c", "-", 16)
        lines = good.splitlines()
        body = base64.b64decode("".join(lines[1:-1]))
        wrap = lambda raw: "-----BEGIN OPENSSH PRIVATE KEY-----\n" + base64.b64encode(raw).decode() + "\n-----END OPENSSH PRIVATE KEY-----\n"   # noqa: E731
        flipped = bytearray(body)
        flipped[-3] ^= 0x55                                             # damage inside the private section: padding or check bytes
        cases = {"empty": "", "no end": good.split("-----END")[0], "not base64": wrap(b"").replace("\n\n", "\n!!!!\n"),
                 "bad magic": wrap(b"openssh-key-v2\0" + bytes(100)), "pem rsa": "-----BEGIN RSA PRIVATE KEY-----\nMIIB\n-----END RSA PRIVATE KEY-----\n",
                 "truncated": wrap(body[:60]), "bad padding": wrap(bytes(flipped))}
        for name, text in cases.items():
            path = self.dir / "damaged"
            path.write_text(text)
            out = tool("keyunlock", path, check=False)
            self.assertTrue(out.startswith("bad:") or out.startswith("locked("), (name, out))
        rsa = self.dir / "rsa"
        keygen("-q", "-t", "rsa", "-b", "2048", "-N", "", "-f", rsa)
        out = tool("keyunlock", rsa, check=False)
        self.assertTrue(out.startswith("bad:") or "ed25519" in out, out)                                 # an RSA key is parsed or refused by name, never used
        pem = self.dir / "pem"
        keygen("-q", "-t", "ecdsa", "-N", "", "-m", "PEM", "-f", pem)
        self.assertIn("older PEM format", tool("keyinfo", pem, check=False))


# ------------------------------------------------------------------------------------------------ the test server, checked against the real OpenSSH client

SSH = shutil.which("ssh")


def new_key(directory, name="id", passphrase=""):
    """An ed25519 key pair made by ssh-keygen; returns (private key path, public key bytes (32))."""
    path = Path(directory) / name
    keygen("-q", "-t", "ed25519", "-N", passphrase, "-C", name, "-f", path)
    pub = base64.b64decode((Path(directory) / (name + ".pub")).read_text().split()[1])[-32:]
    return path, pub


class OpenSshAgainstTestServer(unittest.TestCase):
    """If the OpenSSH client can log in to tests/sshsrv.py and use it, the server's reading of the protocol is the real one; the client under test is then
    held to that server, not only to the OS's own sshd."""

    @classmethod
    def setUpClass(cls):
        import sshsrv
        cls.sshsrv = sshsrv
        cls.dir = Path(tempfile.mkdtemp(prefix="sshc-os-"))
        cls.key, cls.pub = new_key(cls.dir, "alice")
        cls.known = cls.dir / "known_hosts"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def ssh(self, srv, *cmd, user="alice", extra=(), input=None, env=None, timeout=60):
        args = [SSH, "-p", str(srv.port), "-i", str(self.key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                "-o", f"UserKnownHostsFile={self.known}", "-o", "LogLevel=ERROR", *extra, f"{user}@127.0.0.1", *cmd]
        return subprocess.run(args, capture_output=True, input=input, timeout=timeout, env=env)

    def test_exec_status_and_streams(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.ssh(srv, "echo hello world")
            self.assertEqual((r.returncode, r.stdout), (0, b"hello world\n"), r.stderr)
            self.assertEqual(self.ssh(srv, "status 7").returncode, 7)
            r = self.ssh(srv, "both")
            self.assertEqual((r.returncode, r.stdout, r.stderr), (4, b"to-out\n", b"to-err\n"))
            r = self.ssh(srv, "gen 300000")
            self.assertEqual(r.stdout, bytes((i * 7 + 3) & 255 for i in range(300000)))
            self.assertIn(("exec", "echo hello world"), srv.events)
            self.assertEqual(self.ssh(srv, "echo x", user="mallory").returncode, 255)             # not a listed user
            self.assertTrue(any(e[:3] == ("auth", "mallory", "publickey") and not e[3] for e in srv.events) or srv.find("auth-query"))

    def test_bulk_data_and_rekeying(self):
        data = os.urandom(2_000_000)
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.ssh(srv, "sum", input=data, extra=("-o", "RekeyLimit=256K"))
            self.assertEqual(r.stdout.decode().split(), [str(len(data)), hashlib.sha256(data).hexdigest()], r.stderr)
            self.assertGreaterEqual(len(srv.find("kex")), 3)                                      # the client's own re-keys happened
            r = self.ssh(srv, "cat", input=data[:700_000], extra=("-o", "RekeyLimit=128K"))
            self.assertEqual(r.stdout, data[:700_000])

    def test_server_started_rekey(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.ssh(srv, "rekey")
            self.assertEqual((r.returncode, r.stdout), (0, b"before-rekey\nafter-rekey\n"), r.stderr)
            self.assertEqual(len(srv.find("kex")), 2)
        with self.sshsrv.Server(keys={"alice": [self.pub]}, rekey_every=100_000) as srv:        # and one the server wants because of the bytes moved
            r = self.ssh(srv, "gen 500000")
            self.assertEqual(len(r.stdout), 500000)
            self.assertGreaterEqual(len(srv.find("kex")), 3)

    def test_password_through_askpass(self):
        askpass = self.dir / "askpass.sh"
        askpass.write_text("#!/bin/sh\necho 's3cret pw'\n")
        os.chmod(askpass, 0o755)
        env = dict(os.environ, SSH_ASKPASS=str(askpass), SSH_ASKPASS_REQUIRE="force", DISPLAY=":0")
        with self.sshsrv.Server(users={"bob": "s3cret pw"}) as srv:
            args = [SSH, "-p", str(srv.port), "-o", "PubkeyAuthentication=no", "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={self.known}",
                    "-o", "LogLevel=ERROR", "bob@127.0.0.1", "echo logged-in-by-password"]
            r = subprocess.run(args, capture_output=True, env=env, timeout=60)
            self.assertEqual((r.returncode, r.stdout), (0, b"logged-in-by-password\n"), r.stderr)
            wrong = dict(env)
            askpass.write_text("#!/bin/sh\necho wrong\n")
            r = subprocess.run(args, capture_output=True, env=wrong, timeout=60)
            self.assertEqual(r.returncode, 255)

    def test_keyboard_interactive_and_banner(self):
        askpass = self.dir / "askpass-kbd.sh"
        askpass.write_text("#!/bin/sh\ncase \"$1\" in *Code*) echo 4711;; *) echo hunter2;; esac\n")
        os.chmod(askpass, 0o755)
        env = dict(os.environ, SSH_ASKPASS=str(askpass), SSH_ASKPASS_REQUIRE="force", DISPLAY=":0")
        with self.sshsrv.Server(kbdint={"carol": [("Password: ", False, "hunter2"), ("Code: ", True, "4711")]}, banner="Welcome, mind the step.\n") as srv:
            args = [SSH, "-p", str(srv.port), "-o", "PubkeyAuthentication=no", "-o", "PasswordAuthentication=no", "-o", "StrictHostKeyChecking=no",
                    "-o", f"UserKnownHostsFile={self.known}", "carol@127.0.0.1", "echo kbd-ok"]
            r = subprocess.run(args, capture_output=True, env=env, timeout=60)
            self.assertEqual((r.returncode, r.stdout), (0, b"kbd-ok\n"), r.stderr)
            self.assertIn(b"Welcome, mind the step.", r.stderr)

    def test_direct_tcpip_and_pty(self):
        import socketserver
        import threading

        class Echo(socketserver.BaseRequestHandler):
            def handle(self):
                while True:
                    d = self.request.recv(4096)
                    if not d:
                        return
                    self.request.sendall(d.upper())
        echo = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo)
        echo.daemon_threads = True
        threading.Thread(target=echo.serve_forever, daemon=True).start()
        try:
            with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
                import socket
                local = socket.socket()
                local.bind(("127.0.0.1", 0))
                lport = local.getsockname()[1]
                local.close()
                p = subprocess.Popen([SSH, "-N", "-L", f"{lport}:127.0.0.1:{echo.server_address[1]}", "-p", str(srv.port), "-i", str(self.key), "-o", "BatchMode=yes",
                                      "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={self.known}", "-o", "ExitOnForwardFailure=yes", "alice@127.0.0.1"])
                try:
                    deadline = __import__("time").time() + 20
                    while True:
                        try:
                            s = socket.create_connection(("127.0.0.1", lport), timeout=5)
                            break
                        except OSError:
                            self.assertLess(__import__("time").time(), deadline)
                            __import__("time").sleep(0.2)
                    s.sendall(b"through the tunnel")
                    got = b""
                    while len(got) < 18:
                        got += s.recv(100)
                    self.assertEqual(got, b"THROUGH THE TUNNEL")
                    s.close()
                finally:
                    p.kill()
                self.assertTrue(srv.find("open"))
        finally:
            echo.shutdown()


# ------------------------------------------------------------------------------------------------ the client programs against the test server

class ClientBase(unittest.TestCase):
    """A fresh HOME (with ~/.ssh) for the client, a key pair the test server lists, and helpers to run the built `ssh` and friends."""

    @classmethod
    def setUpClass(cls):
        import sshsrv
        cls.sshsrv = sshsrv
        cls.home = Path(tempfile.mkdtemp(prefix="sshc-home-"))
        (cls.home / ".ssh").mkdir(mode=0o700)
        cls.key, cls.pub = new_key(cls.home / ".ssh", "id_ed25519")
        cls.ssh_bin = program("ssh")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.home, ignore_errors=True)

    def setUp(self):
        self.known_hosts().unlink(missing_ok=True)                                       # every test starts without remembered hosts and without a config file
        (self.home / ".ssh" / "config").unlink(missing_ok=True)

    def env(self, **extra):
        env = dict(os.environ, HOME=str(self.home), ASAN_OPTIONS="detect_leaks=0", TERM="xterm")
        env.pop("SSH_ASKPASS", None)
        env.update(extra)
        return env

    def known_hosts(self):
        return self.home / ".ssh" / "known_hosts"

    def run_ssh(self, srv, *args, input=None, env=None, timeout=60, strict="accept-new", user="alice", host="127.0.0.1", prog=None, check_stderr=False):
        cmd = [str(prog or self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), "-o", f"StrictHostKeyChecking={strict}", "-o", "BatchMode=yes", *args]
        # the destination goes before the command: callers put options first and the command last, split at the first non-option
        return subprocess.run(cmd, input=input, capture_output=True, env=env or self.env(), timeout=timeout)

    def login(self, srv, *cmd, **kw):
        dest = f"{kw.pop('user', 'alice')}@127.0.0.1"
        return self.run_ssh(srv, *kw.pop("opts", ()), dest, *cmd, **kw)


@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientExec(ClientBase):
    def test_exec_status_streams_and_output(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.login(srv, "echo", "hello", "world")
            self.assertEqual((r.returncode, r.stdout), (0, b"hello world\n"), r.stderr)
            self.assertIn(b"Permanently added", r.stderr)                                 # accept-new said so
            self.assertEqual(self.login(srv, "status", "7").returncode, 7)
            r = self.login(srv, "both")
            self.assertEqual((r.returncode, r.stdout, r.stderr.endswith(b"to-err\n")), (4, b"to-out\n", True), r.stderr)
            r = self.login(srv, "gen", "300000")
            self.assertEqual(r.stdout, bytes((i * 7 + 3) & 255 for i in range(300000)), r.stderr)


# ------------------------------------------------------------------------------------------------ SFTP: scp and sftp

def tree_digest(root):
    """name -> sha256 of every file under root (relative names with /), plus the directories as 'name/'; for comparing trees."""
    out = {}
    for p in sorted(Path(root).rglob("*")):
        rel = p.relative_to(root).as_posix()
        out[rel + "/" if p.is_dir() else rel] = None if p.is_dir() else hashlib.sha256(p.read_bytes()).hexdigest()
    return out


@unittest.skipUnless(have_gcc() and KEYGEN and SSH and shutil.which("scp") and shutil.which("sftp"), "needs gcc and the OpenSSH tools")
class OpenSshSftpAgainstTestServer(ClientBase):
    """The test server's SFTP, checked with OpenSSH's own scp and sftp (scp speaks SFTP since OpenSSH 9)."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="sshc-sftproot-"))
        self.work = Path(tempfile.mkdtemp(prefix="sshc-work-"))

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)
        shutil.rmtree(self.work, ignore_errors=True)

    def common(self, srv):
        return ["-P", str(srv.port), "-i", str(self.key), "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={self.work / 'kh'}", "-q"]

    def test_scp_and_sftp(self):
        data = os.urandom(1_300_000)
        (self.work / "up.bin").write_bytes(data)
        with self.sshsrv.Server(keys={"alice": [self.pub]}, sftp_root=self.root) as srv:
            r = subprocess.run([shutil.which("scp"), *self.common(srv), str(self.work / "up.bin"), "alice@127.0.0.1:up.bin"], capture_output=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual((self.root / "up.bin").read_bytes(), data)
            r = subprocess.run([shutil.which("scp"), *self.common(srv), "alice@127.0.0.1:up.bin", str(self.work / "down.bin")], capture_output=True, timeout=120)
            self.assertEqual((r.returncode, (self.work / "down.bin").read_bytes() == data), (0, True), r.stderr)
            batch = self.work / "batch"
            batch.write_text("pwd\nmkdir sub\nput %s sub/two.bin\nls -l sub\nrename sub/two.bin sub/three.bin\nchmod 600 sub/three.bin\nget sub/three.bin %s\nrm sub/three.bin\nrmdir sub\n"
                             % ((self.work / "up.bin").as_posix(), (self.work / "three.bin").as_posix()))
            r = subprocess.run([shutil.which("sftp"), "-b", str(batch), *self.common(srv)[:-1], "alice@127.0.0.1"], capture_output=True, timeout=120)
            self.assertEqual(r.returncode, 0, (r.stdout, r.stderr))
            self.assertEqual((self.work / "three.bin").read_bytes(), data)
            self.assertFalse((self.root / "sub").exists())


@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientScpSftp(ClientBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.scp_bin = program("scp")
        cls.sftp_bin = program("sftp")

    def setUp(self):
        super().setUp()
        self.root = Path(tempfile.mkdtemp(prefix="sshc-sftproot-"))
        self.work = Path(tempfile.mkdtemp(prefix="sshc-work-"))
        self.srv =self.sshsrv.Server(keys={"alice": [self.pub]}, sftp_root=self.root)

    def tearDown(self):
        self.srv.stop()
        shutil.rmtree(self.root, ignore_errors=True)
        shutil.rmtree(self.work, ignore_errors=True)

    def scp(self, *args, timeout=120):
        cmd = [str(self.scp_bin), "-F", "/dev/null", "-P", str(self.srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", *map(str, args)]
        return subprocess.run(cmd, capture_output=True, env=self.env(), timeout=timeout)

    def sftp(self, batch_lines, *extra, timeout=120):
        batch = self.work / "batch.txt"
        batch.write_text("\n".join(batch_lines) + "\n")
        cmd = [str(self.sftp_bin), "-F", "/dev/null", "-P", str(self.srv.port), "-o", "StrictHostKeyChecking=accept-new", "-b", str(batch), *extra, "alice@127.0.0.1"]
        return subprocess.run(cmd, capture_output=True, env=self.env(), timeout=timeout)

    def test_scp_files_both_ways(self):
        data = os.urandom(1_500_000)
        up = self.work / "up.bin"
        up.write_bytes(data)
        r = self.scp(up, "alice@127.0.0.1:up.bin")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.root / "up.bin").read_bytes(), data)
        (self.root / "dir").mkdir()
        r = self.scp(up, "alice@127.0.0.1:dir")                       # an existing directory: the file goes into it
        self.assertEqual((r.returncode, (self.root / "dir" / "up.bin").exists()), (0, True), r.stderr)
        down = self.work / "down.bin"
        r = self.scp("alice@127.0.0.1:up.bin", down)
        self.assertEqual((r.returncode, down.read_bytes() == data), (0, True), r.stderr)
        r = self.scp("alice@127.0.0.1:up.bin", self.work)             # into a local directory
        self.assertEqual((r.returncode, (self.work / "up.bin").read_bytes() == data), (0, True), r.stderr)
        r = self.scp("alice@127.0.0.1:nonexistent.txt", self.work / "x")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"nonexistent.txt: No such file", r.stderr)
        r = self.scp(self.work / "nope", "alice@127.0.0.1:x")
        self.assertEqual(r.returncode, 1)
        r = self.scp(self.work, "alice@127.0.0.1:x")                  # a directory needs -r
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"not a regular file", r.stderr)

    def test_scp_recursive_wildcards_and_preserving(self):
        src = self.work / "tree"
        (src / "a" / "b").mkdir(parents=True)
        (src / "top.txt").write_text("top\n")
        (src / "a" / "mid.txt").write_text("mid\n")
        (src / "a" / "b" / "deep.bin").write_bytes(os.urandom(70000))
        os.utime(src / "top.txt", (1_000_000_000, 1_100_000_000))
        os.chmod(src / "top.txt", 0o600)
        r = self.scp("-r", "-p", src, "alice@127.0.0.1:copy")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(tree_digest(src), tree_digest(self.root / "copy"))
        self.assertEqual(int((self.root / "copy" / "top.txt").stat().st_mtime), 1_100_000_000)           # -p kept the time ...
        self.assertEqual(oct((self.root / "copy" / "top.txt").stat().st_mode & 0o777), "0o600")             # ... and the mode
        back = self.work / "back"
        r = self.scp("-r", "-p", "alice@127.0.0.1:copy", back)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(tree_digest(src), tree_digest(back))
        self.assertEqual(int((back / "top.txt").stat().st_mtime), 1_100_000_000)
        (self.root / "w1.log").write_text("1")
        (self.root / "w2.log").write_text("22")
        (self.root / "other.txt").write_text("x")
        got = self.work / "logs"
        got.mkdir()
        r = self.scp("alice@127.0.0.1:*.log", got)
        self.assertEqual((r.returncode, sorted(p.name for p in got.iterdir())), (0, ["w1.log", "w2.log"]), r.stderr)
        r = self.scp("alice@127.0.0.1:*.log", self.work / "notadir.txt")      # two files into one name
        self.assertEqual(r.returncode, 1)

    def test_sftp_batch(self):
        data = os.urandom(200_000)
        (self.work / "f.bin").write_bytes(data)
        r = self.sftp(["pwd", "mkdir work", "cd work", "pwd", f"put {(self.work / 'f.bin').as_posix()} g.bin", "ls -l", "rename g.bin h.bin", "chmod 600 h.bin",
                       "ln -s h.bin link", "get h.bin " + (self.work / "back.bin").as_posix(), "rm link", "rm h.bin", "cd ..", "rmdir work", "lpwd", "bye"])
        out = r.stdout.decode()
        self.assertEqual(r.returncode, 0, (out, r.stderr))
        self.assertIn("Remote working directory: /\n", out)
        self.assertIn("Remote working directory: /work\n", out)
        self.assertRegex(out, r"-rw[-r]\S* .* g\.bin")
        self.assertEqual((self.work / "back.bin").read_bytes(), data)
        self.assertFalse((self.root / "work").exists())

    def test_sftp_batch_stops_at_the_first_failure(self):
        r = self.sftp(["mkdir one", "rmdir nothere", "mkdir two"])
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertTrue((self.root / "one").is_dir())
        self.assertFalse((self.root / "two").exists())                    # never reached
        r = self.sftp(["mkdir three", "-rmdir nothere", "mkdir four"])    # a leading - ignores the failure
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue((self.root / "four").is_dir())
        r = self.sftp(["frobnicate"])
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"Invalid command", r.stdout)
        r = self.sftp(["get missing.bin"])
        self.assertEqual(r.returncode, 1)


# ------------------------------------------------------------------------------------------------ port forwarding

def free_tcp_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class UpperEcho:
    """A TCP service on 127.0.0.1 that answers every byte upper-cased; the thing that gets forwarded to."""

    def __init__(self):
        import socketserver
        import threading

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                while True:
                    d = self.request.recv(65536)
                    if not d:
                        return
                    self.request.sendall(d.upper())
        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


def connect_when_ready(port, timeout=20):
    import socket
    import time
    end = time.time() + timeout
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=10)
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.1)


def read_exactly(sock, n, timeout=30):
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientForwarding(ClientBase):
    def setUp(self):
        super().setUp()
        self.echo = UpperEcho()
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.kill()
            p.wait()
        self.echo.stop()

    def start_ssh(self, srv, *args):
        cmd = [str(self.ssh_bin), "-F", "/dev/null", "-N", "-p", str(srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", *args, "alice@127.0.0.1"]
        p = subprocess.Popen(cmd, env=self.env(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def test_local_forward_with_bulk_data_and_several_connections(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            lport = free_tcp_port()
            self.start_ssh(srv, "-L", f"{lport}:127.0.0.1:{self.echo.port}")
            s = connect_when_ready(lport)
            s.sendall(b"through the tunnel")
            self.assertEqual(read_exactly(s, 18), b"THROUGH THE TUNNEL")
            other = connect_when_ready(lport)                                                   # a second connection at the same time
            other.sendall(b"second")
            self.assertEqual(read_exactly(other, 6), b"SECOND")
            data = bytes(65 + (i * 7) % 26 for i in range(3_000_000))                           # 3 MB each way, both directions in flight at once
            import threading
            sender = threading.Thread(target=lambda: s.sendall(data), daemon=True)
            sender.start()
            got = read_exactly(s, len(data), timeout=120)
            sender.join(60)
            self.assertEqual(got, data.upper())
            s.shutdown(1)                                                                       # half-close: the far side sees the end, the other connection lives
            other.sendall(b"still alive")
            self.assertEqual(read_exactly(other, 11), b"STILL ALIVE")
            other.close()
            self.assertTrue(any(e[:2] == ("open", "direct-tcpip") and e[3] == self.echo.port for e in srv.events), srv.events)

    def test_refused_target_and_prohibited_forwarding(self):
        dead_port = free_tcp_port()
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            lport = free_tcp_port()
            p = self.start_ssh(srv, "-L", f"{lport}:127.0.0.1:{dead_port}")
            s = connect_when_ready(lport)
            self.assertEqual(read_exactly(s, 1, timeout=20), b"")                                 # the target refused: the connection is closed on us
            s.close()
            p.kill()
            p.wait()
            err = p.stderr.read().decode()
            self.assertIn("open failed", err)
        with self.sshsrv.Server(keys={"alice": [self.pub]}, forward_ok=False) as srv:
            lport = free_tcp_port()
            p = self.start_ssh(srv, "-L", f"{lport}:127.0.0.1:{self.echo.port}")
            s = connect_when_ready(lport)
            self.assertEqual(read_exactly(s, 1, timeout=20), b"")
            p.kill()
            p.wait()
            self.assertIn("administratively prohibited", p.stderr.read().decode())

    def test_exit_on_forward_failure(self):
        import socket
        busy = socket.socket()
        busy.bind(("127.0.0.1", 0))
        busy.listen(1)
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            cmd = [str(self.ssh_bin), "-F", "/dev/null", "-N", "-p", str(srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes",
                   "-o", "ExitOnForwardFailure=yes", "-L", f"{busy.getsockname()[1]}:127.0.0.1:{self.echo.port}", "alice@127.0.0.1"]
            r = subprocess.run(cmd, capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"Could not request local forwarding", r.stderr)
        busy.close()

    def socks5(self, port, host, dest_port):
        import socket
        s = connect_when_ready(port)
        s.sendall(b"\x05\x01\x00")
        self.assertEqual(read_exactly(s, 2), b"\x05\x00")
        if isinstance(host, bytes):
            s.sendall(b"\x05\x01\x00\x01" + host + dest_port.to_bytes(2, "big"))
        else:
            h = host.encode()
            s.sendall(b"\x05\x01\x00\x03" + bytes([len(h)]) + h + dest_port.to_bytes(2, "big"))
        reply = read_exactly(s, 10)
        return s, reply

    def test_dynamic_forward_socks_versions(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            dport = free_tcp_port()
            self.start_ssh(srv, "-D", str(dport))
            s, reply = self.socks5(dport, bytes([127, 0, 0, 1]), self.echo.port)               # SOCKS5 with an IPv4 address
            self.assertEqual(reply[:2], b"\x05\x00")
            s.sendall(b"socks five")
            self.assertEqual(read_exactly(s, 10), b"SOCKS FIVE")
            s.close()
            s, reply = self.socks5(dport, "localhost", self.echo.port)                         # ... and with a name (resolved at the far end)
            self.assertEqual(reply[:2], b"\x05\x00")
            s.sendall(b"by name")
            self.assertEqual(read_exactly(s, 7), b"BY NAME")
            s.close()
            s = connect_when_ready(dport)                                                       # SOCKS4
            s.sendall(b"\x04\x01" + self.echo.port.to_bytes(2, "big") + bytes([127, 0, 0, 1]) + b"user\0")
            self.assertEqual(read_exactly(s, 8)[:2], b"\x00\x5a")
            s.sendall(b"four")
            self.assertEqual(read_exactly(s, 4), b"FOUR")
            s.close()
            s = connect_when_ready(dport)                                                       # SOCKS4a: the name follows the user id
            s.sendall(b"\x04\x01" + self.echo.port.to_bytes(2, "big") + b"\x00\x00\x00\x01" + b"user\0localhost\0")
            self.assertEqual(read_exactly(s, 8)[:2], b"\x00\x5a")
            s.sendall(b"four a")
            self.assertEqual(read_exactly(s, 6), b"FOUR A")
            s.close()
            s, reply = self.socks5(dport, bytes([127, 0, 0, 1]), free_tcp_port())              # a closed port: "connection refused"-like failure, not a hang
            self.assertEqual(reply[0], 5)
            self.assertNotEqual(reply[1], 0)
            s.close()
            s = connect_when_ready(dport)
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")                                                 # not SOCKS at all: dropped
            self.assertEqual(read_exactly(s, 1, timeout=20), b"")

    def test_remote_forward(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            rport = free_tcp_port()
            self.start_ssh(srv, "-R", f"{rport}:127.0.0.1:{self.echo.port}")
            s = connect_when_ready(rport)                                                       # the SERVER's port: connections to it come back to this PC
            s.sendall(b"remote forward")
            self.assertEqual(read_exactly(s, 14), b"REMOTE FORWARD")
            data = os.urandom(400_000).replace(b"\x00", b"a")
            import threading
            t = threading.Thread(target=lambda: s.sendall(data), daemon=True)
            t.start()
            self.assertEqual(read_exactly(s, len(data), timeout=60), data.upper())
            s.close()
            self.assertTrue(srv.find("global"))

    def test_forward_refused_by_the_server(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}, forward_ok=False) as srv:
            cmd = [str(self.ssh_bin), "-F", "/dev/null", "-N", "-p", str(srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes",
                   "-o", "ExitOnForwardFailure=yes", "-R", f"{free_tcp_port()}:127.0.0.1:{self.echo.port}", "alice@127.0.0.1"]
            r = subprocess.run(cmd, capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"remote port forwarding failed", r.stderr)



# ------------------------------------------------------------------------------------------------ the terminal: interactive sessions on a pseudo-terminal

class PtyProc:
    """A program run on a pseudo-terminal that the test types at and reads from (the client believes it is on a real console)."""

    def __init__(self, argv, env, rows=24, cols=80):
        import fcntl
        import pty
        import struct
        import termios
        self._fcntl, self._termios, self._struct = fcntl, termios, struct
        pid, fd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            os.execve(argv[0], argv, env)
            os._exit(127)
        self.pid, self.fd = pid, fd
        self.buf = b""
        self.pos = 0
        self.status = None

    def _read(self, timeout):
        import select
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                data = b""
            if data:
                self.buf += data
                return True
        return False

    def text(self):
        return self.buf.decode("utf-8", "replace")

    def expect(self, pattern, timeout=30):
        import time
        end = time.time() + timeout
        rx = re.compile(pattern, re.S)
        while True:
            m = rx.search(self.text(), self.pos)
            if m:
                self.pos = m.end()
                return m
            if time.time() > end:
                raise AssertionError("/%s/ never appeared; the terminal shows:\n%s" % (pattern, self.text()[-1500:]))
            self._read(0.1)

    def absent(self, pattern, for_seconds=1.0):
        import time
        end = time.time() + for_seconds
        while time.time() < end:
            self._read(0.1)
        assert not re.search(pattern, self.text()[self.pos:], re.S), self.text()[-800:]

    def send(self, data):
        os.write(self.fd, data if isinstance(data, bytes) else data.encode())

    def lflag(self):
        return self._termios.tcgetattr(self.fd)[3]

    def resize(self, rows, cols):
        self._fcntl.ioctl(self.fd, self._termios.TIOCSWINSZ, self._struct.pack("HHHH", rows, cols, 0, 0))

    def wait(self, timeout=30):
        import time
        end = time.time() + timeout
        while True:
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(st)
                while self._read(0.05):
                    pass
                return self.status
            if time.time() > end:
                raise AssertionError("the program did not end; the terminal shows:\n" + self.text()[-1500:])
            self._read(0.1)

    def kill(self):
        if self.status is None:
            try:
                os.kill(self.pid, 9)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
            self.status = -9
        try:
            os.close(self.fd)
        except OSError:
            pass


@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientInteractive(ClientBase):
    def setUp(self):
        super().setUp()
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.kill()

    def spawn(self, srv, *args, cmd=(), user="alice", rows=24, cols=80, **env):
        argv = [str(self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), *args, f"{user}@127.0.0.1", *cmd]
        p = PtyProc(argv, self.env(**env), rows, cols)
        self.procs.append(p)
        return p

    def test_shell_session_in_raw_mode(self):
        import termios
        with self.sshsrv.Server(keys={"alice": [self.pub]}, banner=None) as srv:
            p = self.spawn(srv, "-o", "StrictHostKeyChecking=accept-new", rows=24, cols=80)
            p.expect(r"Welcome to the test server\r\nsrv\$ ")
            raw = p.lflag()
            self.assertFalse(raw & (termios.ICANON | termios.ECHO | termios.ISIG), "the terminal must be raw during the session")
            p.send(b"echo hello there\r")
            p.expect(r"hello there\r\nsrv\$ ")                                              # the server echoed what was typed (the local terminal does not)
            p.send(b"size\r")
            p.expect(r"size=80x24\r\n")
            p.resize(40, 100)                                                               # the kernel signals the program, which tells the server
            srv.wait_for("winch")
            p.send(b"size\r")
            p.expect(r"size=100x40\r\n")
            p.send(b"term\r")
            p.expect(r"term=xterm\r\n")
            p.send(b"\x03")
            p.expect(r"\^C")                                                                # Ctrl+C is data for the far side, not a signal for ssh
            p.send(b"\x7fecho back\r")
            p.send(b"exit 3\r")
            self.assertEqual(p.wait(), 3)
            self.assertTrue(p.lflag() & termios.ICANON and p.lflag() & termios.ECHO, "the terminal must be cooked again")
            self.assertIn("Connection to 127.0.0.1 closed.", p.text())
            pty_event = srv.find("pty")[0]
            self.assertEqual(pty_event[1:4], ("xterm", 80, 24))
            modes = pty_event[4]                                                           # RFC 4254 section 8: opcode, uint32 value, ..., 0
            ops, i = {}, 0
            while modes[i] != 0:
                ops[modes[i]] = int.from_bytes(modes[i + 1:i + 5], "big")
                i += 5
            self.assertEqual((ops[1], ops[3]), (3, 127))                                   # VINTR is ^C and VERASE is DEL, as in this terminal
            self.assertEqual((ops[128], ops[129]), (38400, 38400))

    def test_escape_character(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            p = self.spawn(srv, "-o", "StrictHostKeyChecking=accept-new")
            p.expect(r"srv\$ ")
            p.send(b"echo a~b\r")                                                           # a tilde in the middle of a line is nothing special
            p.expect(r"a~b\r\n")
            p.send(b"~?")
            p.expect(r"Supported escape sequences:.*~\.\s+- terminate connection.*~\?\s+- this message.*~~\s+- send the escape character")
            p.send(b"~~x\r")                                                                # at the start of a line ~~ is a single ~
            p.expect(r"unknown: ~x")
            p.send(b"~#")
            p.expect(r"The following connections are open")
            p.send(b"~R")
            p.expect(r"Rekeying")
            srv.wait_for("kex", count=2)
            p.send(b"echo after rekey\r")
            p.expect(r"after rekey\r\n")
            p.send(b"~.")                                                                   # at the start of a line: ends the session at once
            self.assertEqual(p.wait(), 255)
            self.assertIn("Connection to 127.0.0.1 closed.", p.text())
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:                       # -e none: the tilde means nothing
            p = self.spawn(srv, "-o", "StrictHostKeyChecking=accept-new", "-e", "none")
            p.expect(r"srv\$ ")
            p.send(b"~.\r")
            p.expect(r"unknown: ~\.")
            p.send(b"exit\r")
            self.assertEqual(p.wait(), 0)

    def test_unknown_host_is_asked_about(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            p = self.spawn(srv, cmd=["echo", "went-on"])
            p.expect(r"The authenticity of host '\[127\.0\.0\.1\]:%d \(127\.0\.0\.1\)' can't be established\." % srv.port)
            fp = p.expect(r"ED25519 key fingerprint is (SHA256:\S+)\.").group(1)
            self.assertEqual(fp, srv.fingerprint)
            p.expect(r"Are you sure you want to continue connecting \(yes/no/\[fingerprint\]\)\? ")
            p.send(b"maybe\r")
            p.expect(r"Please type 'yes', 'no' or the fingerprint: ")
            p.send(b"no\r")
            p.expect(r"Host key verification failed\.")
            self.assertEqual(p.wait(), 255)
            self.assertFalse(self.known_hosts().exists())                                   # nothing was remembered
            p = self.spawn(srv, cmd=["echo", "went-on"])
            p.expect(r"\(yes/no/\[fingerprint\]\)\? ")
            p.send(fp.encode() + b"\r")                                                      # the fingerprint itself is an answer
            p.expect(r"Permanently added '\[127\.0\.0\.1\]:%d' \(ED25519\)" % srv.port)
            p.expect(r"went-on")
            self.assertEqual(p.wait(), 0)
            self.assertEqual(self.known_hosts().read_text().split()[0], f"[127.0.0.1]:{srv.port}")
            p = self.spawn(srv, cmd=["echo", "quiet-now"])                                   # known: no question
            p.expect(r"quiet-now")
            self.assertEqual(p.wait(), 0)
            self.assertNotIn("authenticity", p.text())

    def test_password_prompt_without_echo(self):
        import termios
        with self.sshsrv.Server(users={"bob": "pw with space"}) as srv:
            p = self.spawn(srv, "-o", "PubkeyAuthentication=no", "-o", "StrictHostKeyChecking=accept-new", user="bob", cmd=["echo", "logged-in"])
            p.expect(r"bob@127\.0\.0\.1's password: ")
            self.assertFalse(p.lflag() & termios.ECHO)
            p.send(b"wrong\r")
            p.expect(r"Permission denied, please try again\.\r\nbob@127\.0\.0\.1's password: ")
            p.send(b"pw with space\r")
            p.expect(r"logged-in")
            self.assertEqual(p.wait(), 0)
            self.assertNotIn("pw with space", p.text())
            self.assertNotIn("wrong", p.text().replace("Permission denied", ""))

    def test_passphrase_is_asked_only_for_a_key_the_server_would_take(self):
        key, pub = new_key(self.home, "locked", passphrase="open sesame")
        other, _ = new_key(self.home, "unlisted", passphrase="never asked")
        with self.sshsrv.Server(keys={"alice": [pub]}) as srv:
            p = self.spawn(srv, "-o", "StrictHostKeyChecking=accept-new", "-o", "PasswordAuthentication=no", "-i", str(other), "-i", str(key), cmd=["echo", "unlocked"])
            p.expect(r"Enter passphrase for key '%s': " % re.escape(str(key)))               # the first key (unlisted) never asked for its passphrase
            p.send(b"not it\r")
            p.expect(r"Bad passphrase, try again")
            p.expect(r"Enter passphrase for key")
            p.send(b"open sesame\r")
            p.expect(r"unlocked")
            self.assertEqual(p.wait(), 0)
            self.assertNotIn("never asked", p.text())
            self.assertEqual(p.text().count("Enter passphrase"), 2)
        with self.sshsrv.Server(keys={"alice": [pub]}) as srv:                              # BatchMode: the key is skipped, not asked about
            r = subprocess.run([str(self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), "-i", str(key), "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
                                "alice@127.0.0.1", "echo", "no"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 255)
            self.assertNotIn(b"no\n", r.stdout)

    def test_key_file_that_others_can_read_is_not_used(self):
        key, pub = new_key(self.home, "loose")
        os.chmod(key, 0o644)
        with self.sshsrv.Server(keys={"alice": [pub]}) as srv:
            r = subprocess.run([str(self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), "-i", str(key), "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
                                "alice@127.0.0.1", "echo", "no"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"UNPROTECTED PRIVATE KEY FILE", r.stderr)
            self.assertIn(b"Permission denied", r.stderr)
            os.chmod(key, 0o600)
            r = subprocess.run([str(self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), "-i", str(key), "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
                                "alice@127.0.0.1", "echo", "yes-now"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual((r.returncode, r.stdout), (0, b"yes-now\n"), r.stderr)


# ------------------------------------------------------------------------------------------------ behaviour: configuration, misbehaving servers, timeouts

@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientBehaviour(ClientBase):
    def sshrun(self, srv_or_none, *args, input=None, timeout=60, config=None, **env):
        cmd = [str(self.ssh_bin), *args]
        return subprocess.run(cmd, capture_output=True, input=input, env=self.env(**env), timeout=timeout)

    def test_config_file(self):
        (self.home / ".ssh" / "config").write_text(
            "# a comment\n"
            "Host friend\n"
            "    HostName 127.0.0.1\n"
            "    User alice\n"
            "    IdentityFile ~/.ssh/id_ed25519\n"
            "    StrictHostKeyChecking accept-new\n"
            "Host *\n"
            "    BatchMode yes\n"
            "    ServerAliveInterval 30\n"
            "Host other\n"
            "    User mallory\n")
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            (self.home / ".ssh" / "config").write_text((self.home / ".ssh" / "config").read_text().replace("friend\n", "friend\n    Port %d\n" % srv.port, 1))
            r = self.sshrun(srv, "friend", "echo", "via-alias")
            self.assertEqual((r.returncode, r.stdout), (0, b"via-alias\n"), r.stderr)
            self.assertIn(("auth", "alice", "publickey", True), srv.events)
            r = self.sshrun(srv, "-l", "alice", "-p", str(srv.port), "-o", "HostName=127.0.0.1", "-o", "StrictHostKeyChecking=accept-new", "other", "echo", "cli-wins")
            self.assertEqual((r.returncode, r.stdout), (0, b"cli-wins\n"), r.stderr)                      # -l beats "User mallory" in the file
            r = self.sshrun(srv, "-F", "/dev/null", "-p", str(srv.port), "friend", "echo", "no")             # without the file "friend" is just a name that does not resolve
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"Could not resolve hostname friend", r.stderr)

    def test_config_problems_are_errors_not_silence(self):
        cfg = self.home / "cfg"
        for text, message in (("Frobnicate yes\n", b"Bad configuration option: Frobnicate"), ("ProxyCommand nc %h %p\n", b"ProxyCommand is not supported"),
                              ("Host x\nPort\n", b"missing argument"), ("StrictHostKeyChecking sometimes\n", b"Bad StrictHostKeyChecking")):
            cfg.write_text(text)
            r = self.sshrun(None, "-F", str(cfg), "127.0.0.1", "true")
            self.assertEqual(r.returncode, 255, text)
            self.assertIn(message, r.stderr, text)
        r = self.sshrun(None, "-F", "/nonexistent", "127.0.0.1", "true")
        self.assertEqual(r.returncode, 255)
        r = self.sshrun(None, "-A", "127.0.0.1", "true")
        self.assertEqual(r.returncode, 255)
        self.assertIn(b"agent and X11 forwarding", r.stderr)
        r = self.sshrun(None, "-J", "jump", "127.0.0.1", "true")
        self.assertEqual(r.returncode, 255)
        cfg.write_text("Compression yes\nCiphers aes256-ctr\nControlMaster auto\nForwardAgent yes\n")       # accepted and ignored (agent forwarding stays off, said so)
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.sshrun(None, "-F", str(cfg), "-p", str(srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes", "alice@127.0.0.1", "echo", "ok")
            self.assertEqual((r.returncode, r.stdout), (0, b"ok\n"), r.stderr)
            self.assertIn(b"ForwardAgent and ForwardX11 are not supported", r.stderr)

    def test_a_wrong_key_and_the_method_list(self):
        wrong, _ = new_key(self.home, "wrong")
        with self.sshsrv.Server(keys={"alice": [self.pub]}, users={"alice": "x"}) as srv:
            r = self.sshrun(None, "-F", "/dev/null", "-p", str(srv.port), "-i", str(wrong), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
                         "alice@127.0.0.1", "echo", "no")
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"alice@127.0.0.1: Permission denied (publickey,password).", r.stderr)
            self.assertTrue(any(e[:2] == ("auth-query", "alice") and not e[3] for e in srv.events))

    def test_banner_cannot_move_the_cursor(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}, banner="Hello\x1b[2J\x1b]0;evil\x07 world\nsecond line\n") as srv:
            r = self.login(srv, "echo", "ok")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn(b"\x1b", r.stderr)
            self.assertNotIn(b"\x07", r.stderr)
            self.assertIn(b"Hello?[2J?]0;evil? world\nsecond line\n", r.stderr)

    def test_servers_that_misbehave(self):
        base = ["-F", "/dev/null", "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes"]
        with self.sshsrv.Server(keys={"alice": [self.pub]}, ignore_before_kexinit=True) as srv:           # the Terrapin shape: something before the server's KEXINIT
            r = self.sshrun(None, *base, "-p", str(srv.port), "alice@127.0.0.1", "echo", "no")
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"KEXINIT was not its first packet", r.stderr)
        with self.sshsrv.Server(keys={"alice": [self.pub]}, bad_signature=True) as srv:
            r = self.sshrun(None, *base, "-p", str(srv.port), "alice@127.0.0.1", "echo", "no")
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"host key signature is wrong", r.stderr)
            self.assertFalse(self.known_hosts().exists())
        with self.sshsrv.Server(keys={"alice": [self.pub]}, strict=False) as srv:                         # an old server without strict KEX: allowed
            r = self.sshrun(None, *base, "-p", str(srv.port), "alice@127.0.0.1", "echo", "fine")
            self.assertEqual((r.returncode, r.stdout), (0, b"fine\n"), r.stderr)
        for kw, text in (({"hostkey_algs": ["ssh-rsa"]}, b"no matching host key type found. Their offer: ssh-rsa"),
                         ({"ciphers": ["aes256-ctr"]}, b"no matching cipher found. Their offer: aes256-ctr"),
                         ({"kex_algs": ["diffie-hellman-group14-sha1"]}, b"no matching key exchange method found. Their offer: diffie-hellman-group14-sha1")):
            with self.sshsrv.Server(keys={"alice": [self.pub]}, **kw) as srv:
                r = self.sshrun(None, *base, "-p", str(srv.port), "alice@127.0.0.1", "echo", "no")
                self.assertEqual(r.returncode, 255)
                self.assertIn(text, r.stderr)
        with self.sshsrv.Server(keys={"alice": [self.pub]}, disconnect_after_auth=True) as srv:
            r = self.sshrun(None, *base, "-p", str(srv.port), "alice@127.0.0.1", "echo", "no")
            self.assertEqual(r.returncode, 255)
            self.assertRegex(r.stderr.decode(), r"Received disconnect from 127\.0\.0\.1 port %d:11: bye" % srv.port)
        import socket
        ls = socket.socket()                                                                                   # something that accepts and says nothing: not an SSH server
        ls.bind(("127.0.0.1", 0))
        ls.listen(5)
        r = self.sshrun(None, *base, "-o", "ConnectTimeout=2", "-p", str(ls.getsockname()[1]), "alice@127.0.0.1", "echo", "no", timeout=30)
        ls.close()
        self.assertEqual(r.returncode, 255)
        self.assertRegex(r.stderr.decode(), r"timed out|Timeout")
        r = self.sshrun(None, *base, "-p", str(free_tcp_port()), "alice@127.0.0.1", "echo", "no")                # nothing listening
        self.assertEqual(r.returncode, 255)
        self.assertIn(b"connect to host 127.0.0.1 port", r.stderr)
        self.assertIn(b"Connection refused", r.stderr)

    def test_a_changed_or_revoked_host_key_is_refused(self):
        port = free_tcp_port()
        first = self.sshsrv.Server(keys={"alice": [self.pub]}, port=port)
        r = self.sshrun(None, "-F", "/dev/null", "-p", str(port), "-o", "StrictHostKeyChecking=accept-new", "alice@127.0.0.1", "echo", "first")
        self.assertEqual((r.returncode, r.stdout), (0, b"first\n"), r.stderr)
        first.stop()
        before = self.known_hosts().read_text()
        second = self.sshsrv.Server(keys={"alice": [self.pub]}, port=port)                                  # same address, another key
        for strict in ("accept-new", "no", "yes", "ask"):
            r = self.sshrun(None, "-F", "/dev/null", "-p", str(port), "-o", f"StrictHostKeyChecking={strict}", "-o", "BatchMode=yes", "alice@127.0.0.1", "echo", "no")
            self.assertEqual(r.returncode, 255, strict)
            err = r.stderr.decode()
            self.assertIn("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!", err)
            self.assertIn(second.fingerprint, err)
            self.assertIn(f"{self.known_hosts()}:1", err)
            self.assertTrue(err.rstrip().endswith("Host key verification failed."), err[-200:])
            self.assertNotIn("no\n", r.stdout.decode())
        self.assertEqual(self.known_hosts().read_text(), before)                                           # untouched
        second.stop()
        revoked = self.sshsrv.Server(keys={"alice": [self.pub]}, port=port)
        self.known_hosts().write_text(f"@revoked * {revoked.public_line()}\n")
        r = self.sshrun(None, "-F", "/dev/null", "-p", str(port), "-o", "StrictHostKeyChecking=no", "-o", "BatchMode=yes", "alice@127.0.0.1", "echo", "no")
        self.assertEqual(r.returncode, 255)
        self.assertIn(b"REVOKED HOST KEY", r.stderr)
        revoked.stop()

    def test_hashed_known_hosts_and_the_ssh_keygen_cure(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.login(srv, "echo", "hashed", opts=("-o", "HashKnownHosts=yes"))
            self.assertEqual((r.returncode, r.stdout), (0, b"hashed\n"), r.stderr)
            self.assertTrue(self.known_hosts().read_text().startswith("|1|"))
            self.assertNotIn("127.0.0.1", self.known_hosts().read_text())
            self.assertIn(srv.public_line().split()[1], keygen("-F", f"[127.0.0.1]:{srv.port}", "-f", self.known_hosts()))
            r = self.login(srv, "echo", "again", strict="yes")                                              # a hashed entry is found again
            self.assertEqual((r.returncode, r.stdout), (0, b"again\n"), r.stderr)
        self.known_hosts().write_text("")

    def test_server_that_stops_answering_is_given_up_on(self):
        import time
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            p = subprocess.Popen([str(self.ssh_bin), "-F", "/dev/null", "-p", str(srv.port), "-o", "StrictHostKeyChecking=accept-new", "-o", "BatchMode=yes",
                                  "-o", "ServerAliveInterval=1", "-o", "ServerAliveCountMax=2", "alice@127.0.0.1", "sleep", "120"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env())
            srv.wait_for("exec")
            time.sleep(0.5)
            srv.frozen = True
            t0 = time.time()
            out, err = p.communicate(timeout=30)
            self.assertEqual(p.returncode, 255)
            self.assertIn(b"Timeout, server 127.0.0.1 not responding.", err)
            self.assertLess(time.time() - t0, 15)

    def test_exit_by_signal_and_no_status(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.login(srv, "signal", "TERM")
            self.assertEqual(r.returncode, 255)
            self.assertIn(b"Killed by signal TERM.", r.stderr)
            r = self.login(srv, "noexit")
            self.assertEqual((r.returncode, r.stdout), (255, b"no status\n"))
            r = self.login(srv, "status", "300")                                                          # exit statuses are 8 bits
            self.assertEqual(r.returncode, 300 & 255)

    def test_stdin_and_stdout_stream_in_both_directions(self):
        data = os.urandom(1_000_000)
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = self.login(srv, "cat", input=data, opts=("-o", "RekeyLimit=100K"))
            self.assertEqual((r.returncode, r.stdout == data), (0, True), r.stderr[-300:])
            self.assertGreaterEqual(len(srv.find("kex")), 3)
            r = self.login(srv, "sum", input=data)
            self.assertEqual(r.stdout.decode().split(), [str(len(data)), hashlib.sha256(data).hexdigest()])
            r = self.login(srv, "upper", input=b"", opts=("-n",))                                         # -n: nothing is read from stdin, the far side sees the end at once
            self.assertEqual((r.returncode, r.stdout), (0, b""))
            r = self.login(srv, "env", opts=("-t", "-tt"), input=b"")                                     # -tt without a terminal: a pty is asked for anyway
            self.assertIn(b"pty=('xterm'", r.stdout)


# ------------------------------------------------------------------------------------------------ ssh-keygen and ssh-keyscan as programs

@unittest.skipUnless(have_gcc() and KEYGEN and SSH, "needs gcc and the OpenSSH tools")
class ClientTools(ClientBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.keygen_bin = program("ssh-keygen")
        cls.keyscan_bin = program("ssh-keyscan")

    def mykeygen(self, *args, input=None):
        # a session of its own: with no controlling terminal /dev/tty cannot be opened, so a question ("Overwrite (y/n)?") has no one to ask and is refused
        return subprocess.run([str(self.keygen_bin), *map(str, args)], capture_output=True, env=self.env(), input=input, timeout=120, start_new_session=True)

    def test_generated_keys_are_openssh_keys(self):
        out = self.home / "gen"
        r = self.mykeygen("-t", "ed25519", "-f", out, "-N", "", "-C", "made by the OS")
        self.assertEqual(r.returncode, 0, r.stderr)
        text = r.stdout.decode()
        self.assertIn(f"Your identification has been saved in {out}", text)
        self.assertIn("The key's randomart image is:", text)
        self.assertEqual(oct(out.stat().st_mode & 0o777), "0o600")
        pub = Path(str(out) + ".pub").read_text()
        self.assertTrue(pub.startswith("ssh-ed25519 ") and pub.rstrip().endswith("made by the OS"))
        self.assertEqual(keygen("-y", "-f", out).strip(), pub.strip())                                       # OpenSSH derives the same public key
        want = keygen("-l", "-f", out).strip()                                                               # "256 SHA256:... comment (ED25519)"
        self.assertIn(want.split()[1] + " made by the OS\n", text)                                           # the fingerprint printed is OpenSSH's
        self.assertEqual(keygen("-lv", "-f", out).split("\n", 1)[1].strip(), text.split("randomart image is:\n", 1)[1].strip())
        mine = self.mykeygen("-l", "-f", out)
        self.assertEqual(mine.stdout.decode().strip(), want)
        mine = self.mykeygen("-l", "-f", str(out) + ".pub")
        self.assertEqual(mine.stdout.decode().strip(), want)
        r = self.mykeygen("-t", "ed25519", "-f", out, "-N", "")                                              # no terminal to ask "overwrite?" on: refused
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"already exists", r.stderr)
        self.assertEqual(self.mykeygen("-y", "-f", out).stdout.decode().strip(), pub.strip())

    def test_passphrase_changes_and_interop_with_the_openssh_client(self):
        out = self.home / "pp"
        self.assertEqual(self.mykeygen("-q", "-t", "ed25519", "-f", out, "-N", "first secret", "-C", "pp").returncode, 0)
        self.assertEqual(keygen("-y", "-f", out, "-P", "first secret").split()[0], "ssh-ed25519")           # OpenSSH opens it
        self.assertEqual(self.mykeygen("-p", "-f", out, "-P", "first secret", "-N", "second secret").returncode, 0)
        self.assertNotEqual(subprocess.run([KEYGEN, "-y", "-f", str(out), "-P", "first secret"], capture_output=True).returncode, 0)     # the old passphrase no longer opens it
        self.assertEqual(keygen("-y", "-f", out, "-P", "second secret").split()[0], "ssh-ed25519")
        r = self.mykeygen("-c", "-f", out, "-P", "second secret", "-C", "renamed")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(Path(str(out) + ".pub").read_text().rstrip().endswith("renamed"))
        self.assertEqual(self.mykeygen("-p", "-f", out, "-P", "wrong", "-N", "x" * 8).returncode, 1)
        r = self.mykeygen("-q", "-t", "ed25519", "-f", self.home / "short", "-N", "abc")                       # shorter than five characters
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"too short", r.stderr)
        # a key made here logs in through the real OpenSSH client
        plain = self.home / "plainkey"
        self.mykeygen("-q", "-t", "ed25519", "-f", plain, "-N", "")
        pk = base64.b64decode(Path(str(plain) + ".pub").read_text().split()[1])[-32:]
        with self.sshsrv.Server(keys={"carol": [pk]}) as srv:
            r = subprocess.run([SSH, "-p", str(srv.port), "-i", str(plain), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                                "-o", f"UserKnownHostsFile={self.home / 'kh-o'}", "-o", "LogLevel=ERROR", "carol@127.0.0.1", "echo", "openssh-in"], capture_output=True, timeout=60)
            self.assertEqual((r.returncode, r.stdout), (0, b"openssh-in\n"), r.stderr)

    def test_known_hosts_maintenance(self):
        kh = self.home / "maint_hosts"
        _, pub_a = new_key(self.home, "ka")
        line = lambda host, comment="": f"{host} ssh-ed25519 " + base64.b64encode(b"\0\0\0\x0bssh-ed25519\0\0\0\x20" + pub_a).decode()   # noqa: E731
        kh.write_text(line("alpha") + "\n" + line("beta,gamma") + "\n" + line("[delta]:2222") + "\n")
        r = self.mykeygen("-F", "gamma", "-f", kh)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("# Host gamma found: line 2", r.stdout.decode())
        self.assertEqual(self.mykeygen("-F", "nothere", "-f", kh).returncode, 1)
        self.assertEqual(self.mykeygen("-F", "[delta]:2222", "-f", kh).returncode, 0)
        r = self.mykeygen("-R", "alpha", "-f", kh)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("alpha", kh.read_text())
        self.assertIn("gamma", kh.read_text())
        self.assertTrue(Path(str(kh) + ".old").exists())
        r = self.mykeygen("-H", "-f", kh)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(kh.read_text().count("|1|"), 3)                                                      # beta, gamma and [delta]:2222, one entry each
        self.assertNotIn("gamma", kh.read_text())
        self.assertIn("ssh-ed25519", keygen("-F", "gamma", "-f", kh))                                          # OpenSSH finds a host hashed here
        self.assertIn("ssh-ed25519", keygen("-F", "[delta]:2222", "-f", kh))
        self.assertEqual(self.mykeygen("-F", "beta", "-f", kh).returncode, 0)

    def test_keyscan(self):
        with self.sshsrv.Server(keys={"alice": [self.pub]}) as srv:
            r = subprocess.run([str(self.keyscan_bin), "-p", str(srv.port), "127.0.0.1"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.decode().strip(), f"[127.0.0.1]:{srv.port} " + srv.public_line())
            self.assertRegex(r.stderr.decode(), r"# 127\.0\.0\.1:%d SSH-2\.0-PyTestSrv_1" % srv.port)
            fp = subprocess.run([KEYGEN, "-l", "-f", "-"], input=r.stdout, capture_output=True, timeout=30).stdout.decode()
            self.assertIn(srv.fingerprint, fp)                                                                 # OpenSSH agrees on the fingerprint
            r = subprocess.run([str(self.keyscan_bin), "-q", "-H", "-p", str(srv.port), "127.0.0.1"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.stderr, b"")
            hashed = self.home / "scanned"
            hashed.write_bytes(r.stdout)
            self.assertIn("ssh-ed25519", keygen("-F", f"[127.0.0.1]:{srv.port}", "-f", hashed))
            r = subprocess.run([str(self.keyscan_bin), "-p", str(free_tcp_port()), "127.0.0.1"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 1)
            self.assertIn(b"Connection refused", r.stderr)
        with self.sshsrv.Server(hostkey_algs=["ssh-rsa"]) as srv:
            r = subprocess.run([str(self.keyscan_bin), "-p", str(srv.port), "127.0.0.1"], capture_output=True, env=self.env(), timeout=60)
            self.assertEqual(r.returncode, 1)
            self.assertIn(b"no matching host key type found", r.stderr)


if __name__ == "__main__":
    unittest.main()
