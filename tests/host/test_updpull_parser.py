#!/usr/bin/env python3
"""The C manifest parser of `update pull` (user/bin/kappabox/updmanifest.c), fuzzed on the host and compared with the reference parser of tools/serve-update.py.

    python tools/hosttest.py discover -s tests/host -p "test_updpull_parser.py" -v

The parser is compiled with the host's gcc, AddressSanitizer and UBSan (tests/host/updpull_parser_host.c is the harness), then fed a corpus of valid manifests
written by serve-update.py's own encoder and several thousand mutations of them (fixed seeds: bit flips, truncation, duplicated, deleted and swapped lines,
inserted NUL/CR/space bytes, long fields, numbers at the limits). For every candidate both parsers must agree: both refuse, or both accept and read the same
version, `issued`, key id, files, sizes, hashes, signature bytes and the same signed-body length. A disagreement is a bug in one of them (the signature covers
the body the parser used: two parsers that read one signed body differently would install different things).

The parser does not verify signatures (updpull.c does, with libtls), so the corpus needs no key: the signature line is 128 hex digits of the seed.
"""
import hashlib
import importlib.util
import random
import shutil
import struct
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
spec = importlib.util.spec_from_file_location("serve_update", REPO / "tools" / "serve-update.py")
su = importlib.util.module_from_spec(spec)
spec.loader.exec_module(su)

SRC = REPO / "user" / "bin" / "kappabox"
OUT = REPO / "build" / "host-x86-wsl"
CC = shutil.which("gcc") or shutil.which("cc")


def manifest(rng, version="v1", issued=1792800000, paths=("initrd.img", "kernel.elf"), sizes=None):
    key_id = "%016x" % rng.getrandbits(64)
    ents = [su.Entry(p, sizes[i] if sizes else rng.randrange(1, 1 << 28), hashlib.sha256(b"%d" % rng.getrandbits(64)).hexdigest()) for i, p in enumerate(paths)]
    body = su.encode_body(version, issued, key_id, ents)
    return body + b"signature " + (b"%0128x" % rng.getrandbits(512)) + b"\n"


def corpus(rng):
    c = [manifest(rng),
         manifest(rng, paths=su.ESP_FILES),
         manifest(rng, "0", 0, ("a",), (0,)),
         manifest(rng, "A" + "z" * 63, 10 ** 16 - 1, ("kernel.elf",), (10 ** 12 - 1,)),
         manifest(rng, "1.2.3+g0abc-dirty_x", 1, ("_x/" + "y" * 64, "a.b-c_d/e/f/g/h/i/j/k"), (1, 2)),
         manifest(rng, "v", 5, tuple("f%02d" % i for i in range(16))),
         manifest(rng, "v", 5, ("/".join(["c" * 15] * 8),))]
    for m in c:
        su.parse_manifest(m)                                    # the corpus is valid by the reference
    return c


def mutate(rng, m):
    """One mutation; several kinds work on lines so that the result often stays close to the grammar (where the two parsers could differ)."""
    b = bytearray(m)
    lines = m.split(b"\n")
    k = rng.randrange(16)
    if k == 0 and b:
        i = rng.randrange(len(b)); b[i] ^= 1 << rng.randrange(8)
    elif k == 1:
        del b[rng.randrange(len(b) + 1):]
    elif k == 2:
        i = rng.randrange(len(b) + 1); b[i:i] = bytes([rng.choice([0, 13, 32, 10, 9, 0x7f, 0x80, 0xff, 47, 46])])
    elif k == 3:
        i = rng.randrange(len(lines)); lines.insert(i, lines[rng.randrange(len(lines))]); b = bytearray(b"\n".join(lines))
    elif k == 4 and len(lines) > 1:
        del lines[rng.randrange(len(lines))]; b = bytearray(b"\n".join(lines))
    elif k == 5 and len(lines) > 2:
        i, j = rng.randrange(len(lines) - 1), rng.randrange(len(lines) - 1); lines[i], lines[j] = lines[j], lines[i]; b = bytearray(b"\n".join(lines))
    elif k == 6:                                                # a number at or over a limit, or badly written
        i = rng.randrange(len(lines)); parts = lines[i].split(b" ")
        j = rng.randrange(len(parts))
        parts[j] = rng.choice([b"0", b"00", b"01", b"9" * 12, b"9" * 13, b"1" + b"0" * 12, b"9" * 16, b"9" * 17, b"-1", b"+1", b"1e3", b"0x10", b"", b"18446744073709551616"])
        lines[i] = b" ".join(parts); b = bytearray(b"\n".join(lines))
    elif k == 7:                                                # a long or odd field
        i = rng.randrange(len(lines)); parts = lines[i].split(b" ")
        j = rng.randrange(len(parts))
        parts[j] = rng.choice([b"a" * 64, b"a" * 65, b"a" * 128, b"a/" * 64 + b"a", b"a" * 129, b"..", b".", b"a/../b", b"a//b", b"/a", b"a/", b"a\\b", b"-a", b".a", b"_a", b"A" * 5000,
                               b"/".join([b"d"] * 8), b"/".join([b"d"] * 9), b"f" * 64, b"F" * 64, b"f" * 63, b"f" * 128, b"f" * 127, b"f" * 129, b"f" * 16, b"f" * 15])
        lines[i] = b" ".join(parts); b = bytearray(b"\n".join(lines))
    elif k == 8:
        i = rng.randrange(len(b) + 1); b[i:i] = b[rng.randrange(len(b) + 1):][:rng.randrange(1, 200)]
    elif k == 9:
        b += rng.choice([b"\n", b"x", b"signature " + b"0" * 128 + b"\n", b"end\n", b"\r\n", b"\0"])
    elif k == 10 and b:
        i = rng.randrange(len(b)); b[i] = rng.randrange(256)
    elif k == 11:                                               # an extra field or a doubled space on a line
        i = rng.randrange(len(lines)); lines[i] = rng.choice([lines[i] + b" x", lines[i] + b" ", b" " + lines[i], lines[i].replace(b" ", b"  ", 1), lines[i].replace(b" ", b"\t", 1)])
        b = bytearray(b"\n".join(lines))
    elif k == 12:                                               # many file lines (the limits of 16 files, 64 lines, 4096 bytes)
        extra = [b"file g%03d 1 %s" % (i, b"0" * 64) for i in range(rng.choice([1, 13, 14, 15, 40, 58, 59, 60, 61, 70]))]
        lines[-3:-3] = extra; b = bytearray(b"\n".join(lines))
    elif k == 13:
        b = bytearray(m.replace(b"\n", b"\r\n", rng.choice([1, 100])))
    elif k == 14:
        i = rng.randrange(len(lines)); lines[i] = rng.choice([b"", b"end", b"file", b"signature", b"signature " + b"a" * 128, b"key 0123456789abcdef", b"issued 7", b"version x", b"touhou-update 1", b"touhou-update 2"])
        b = bytearray(b"\n".join(lines))
    else:
        b = bytearray(mutate(rng, mutate(rng, m)))
    return bytes(b)


def reference(data):
    try:
        m = su.parse_manifest(data)
    except su.ManifestError:
        return "R"
    out = ["A", m.version, str(m.issued), m.key_id, str(len(m.body)), m.signature.hex(), str(len(m.files))]
    for e in m.files:
        out += [e.path, str(e.size), e.sha256]
    return " ".join(out)


@unittest.skipUnless(CC, "no C compiler on this host (python tools/hosttest.py runs the test in WSL)")
class TestParser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.exe = OUT / "updpull_parser_host"
        cmd = [CC, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(SRC),
               str(SRC / "updmanifest.c"), str(REPO / "tests" / "host" / "updpull_parser_host.c"), "-o", str(cls.exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError("the parser does not compile on the host:\n" + r.stderr)

    def both(self, candidates):
        feed = b"".join(struct.pack("<I", len(c)) + c for c in candidates)
        r = subprocess.run([str(self.exe)], input=feed, capture_output=True)
        self.assertEqual(r.returncode, 0, "the C parser crashed or a sanitizer fired:\n" + r.stderr.decode("utf-8", "replace")[-3000:])
        got = r.stdout.decode("ascii").splitlines()
        self.assertEqual(len(got), len(candidates))
        accepted = 0
        for c, g in zip(candidates, got):
            want = reference(c)
            self.assertEqual(g, want, f"the parsers disagree on {c!r}")
            accepted += g != "R"
        return accepted

    def test_corpus_is_read_the_same(self):
        c = corpus(random.Random(1))
        self.assertEqual(self.both(c), len(c))

    def test_hand_written_cases(self):
        m = corpus(random.Random(2))[0]
        body, _ = su.split_manifest(m)
        sig = m[len(body):]
        cases = [b"", b"\n", m[:-1], m + b"\n", m + sig, body, body + sig + sig, sig, body + sig.upper(), body.replace(b"\nend\n", b"\n") + sig,
                 body + sig[:-1] + b"\0", b"\0" + m, m.replace(b"\n", b"\r\n"), body + b"signature " + b"g" * 128 + b"\n", body + b"signature  " + b"0" * 127 + b"\n",
                 m.replace(b"issued ", b"issued 0"), m.replace(b"issued ", b"issued -"), m.replace(b"version v1", b"version " + b"v" * 65), m.replace(b"version v1", b"version .v"),
                 m.replace(b"version v1", b"version"), m.replace(b"version v1", b"version "), m.replace(b"file initrd.img", b"file kernel.elf"),
                 m.replace(b"file kernel.elf", b"file initrd.img"), m.replace(b"file initrd.img", b"file ../initrd.img"), m.replace(b"file initrd.img", b"file initrd.img/"),
                 m.replace(b"touhou-update 1", b"touhou-update 1 "), b"x" * 4096 + b"\n", (b"a\n" * 2100)[:4096], b"\n" * 4096,
                 body + b"\n" * (4096 - len(m)) + sig, body + b"\n" * (4097 - len(m)) + sig]
        self.assertEqual(self.both(cases), 0)

    def test_mutations_agree_with_the_reference(self):
        total = accepted = 0
        for seed in (20261124, 7, 98):
            rng = random.Random(seed)
            base = corpus(rng)
            cands = [mutate(rng, rng.choice(base)) for _ in range(4000)]
            accepted += self.both(cands)
            total += len(cands)
        # some mutations must stay valid (a flipped hash digit, a changed number), or the comparison of the accepted fields would test nothing
        self.assertGreater(accepted, total // 200, f"only {accepted} of {total} mutations were accepted")
        print(f"\n  {total} mutations, {accepted} accepted by both parsers, no disagreement", end="")


if __name__ == "__main__":
    unittest.main()
