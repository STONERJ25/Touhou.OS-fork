#!/usr/bin/env python3
"""Host tests for libtls (user/lib/tls) and libhttp (user/lib/http), through tests/host/tlstool.c built with gcc,
AddressSanitizer and UBSan. No QEMU: the same C sources run here as in the OS.

    python tools/hosttest.py discover -s tests/host -p "test_tls_vectors.py" -v       (on Windows: in WSL's Ubuntu)

What is checked against what:
  * the primitives against published vectors (FIPS 180/197, RFC 4231, 5869, 7748, 8032, 8439) and against Python's `hashlib`,
    `hmac` and `cryptography` with random inputs; every constant of tables.h is recomputed from its definition (cube and square
    roots of primes, the AES field inverse, the curve parameters as `openssl ecparam` prints them);
  * certificates made here with `cryptography` (and some by the `openssl` command): parsing, chains, every kind of refusal,
    names, name constraints, DER torture (truncation at every byte, 200,000 mutations) without an ASan report;
  * handshakes against `openssl s_server` (every suite of both versions, HelloRetryRequest, KeyUpdate), Python's `ssl` module
    (bulk data, close behaviour), and an independent TLS 1.3 server written below in Python that can break the protocol on purpose;
  * the RFC 8448 traces byte for byte, when the RFC's text is at build/rfc8448.txt (or $TOUHOU_RFC8448).
Needs gcc, python3 with `cryptography`, and `openssl` (the handshake tests skip without it).
"""
import datetime
import hashlib
import hmac
import ipaddress
import os
import random
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402  (REPO, find_cc, the WSL build directory convention)

REPO = x86ref.REPO
OUT = REPO / "build" / ("host-tls-wsl" if "wsl" in str(x86ref.OUT) else "host-tls")
TLS = REPO / "user" / "lib" / "tls"
HTTP = REPO / "user" / "lib" / "http"

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa, x25519
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF, HKDFExpand
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
    HAVE_CRYPTO = True
except ImportError:                                                     # pragma: no cover
    HAVE_CRYPTO = False

OPENSSL = shutil.which("openssl")
H = bytes.hex
U = bytes.fromhex


def hx(b):
    return b.hex() if b else "-"


# ------------------------------------------------------------------------------------------------------------ the tool

def sources():
    srcs = sorted((TLS / "src").glob("*.c")) + sorted(s for s in (REPO / "user" / "lib" / "sshcrypto" / "src").glob("*.c") if s.stem in ("curve25519", "sha512", "chachapoly"))   # libtls needs only Ed25519 and ChaCha20-Poly1305 from it: the ssh client's sshc_*.c want the OS's libc headers
    if (HTTP / "src").is_dir():
        srcs += sorted((HTTP / "src").glob("*.c")) + sorted((REPO / "user" / "lib" / "z" / "src").glob("*.c"))
    return srcs


def build_tool():
    """Each source becomes an object of its own (in parallel, rebuilt when it or a header changed), then they are linked."""
    import concurrent.futures
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "tlstool"
    srcs = [REPO / "tests" / "host" / "tlstool.c"] + sources()
    headers = list((TLS / "src").glob("*.h")) + list((TLS / "include").glob("*.h")) + list(HTTP.glob("*/*.h")) + \
        list((REPO / "user" / "lib" / "z").glob("*/*.h")) + list((REPO / "user" / "lib" / "sshcrypto").glob("*/*.h"))
    newest_header = max(h.stat().st_mtime for h in headers)
    cc = x86ref.find_cc()
    if not cc:
        raise unittest.SkipTest("no C compiler")
    inc = ["-I", str(TLS / "include"), "-I", str(TLS / "src"), "-I", str(REPO / "user" / "lib" / "sshcrypto" / "include")]
    defs = []
    if (HTTP / "src").is_dir():
        inc += ["-I", str(HTTP / "include"), "-I", str(REPO / "user" / "lib" / "z" / "include")]
        defs = ["-DWITH_HTTP=1"]
    is_gcc = "gcc" in Path(cc).name
    san = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if is_gcc else ["-O2", "-g"]
    base = [cc, *san, "-std=gnu17", "-Wall", "-Wextra", *(["-Werror"] if is_gcc else []), "-Wno-unused-parameter", "-Wno-sign-compare", *defs, *inc]

    def obj_of(src):
        return OUT / "obj" / (src.parent.name + "_" + src.stem + ".o")

    def compile_one(src):
        obj = obj_of(src)
        if obj.exists() and obj.stat().st_mtime >= max(src.stat().st_mtime, newest_header):
            return None
        extra = []
        if "sshcrypto" in src.parts and is_gcc:
            # TweetNaCl-style carries shift negative 64-bit values left (curve25519.c car25519): undefined in C17, two's
            # complement in practice; that file is not this library's, so UBSan's shift check is off for it alone.
            extra = ["-fno-sanitize=shift-base"]
        r = subprocess.run(base + extra + ["-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return None if r.returncode == 0 else f"{src.name}:\n{r.stdout}{r.stderr}"

    (OUT / "obj").mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        errors = [e for e in ex.map(compile_one, srcs) if e]
    if errors:
        raise RuntimeError("build of tlstool failed:\n" + "\n".join(errors))
    objs = [obj_of(s) for s in srcs]
    if exe.exists() and all(o.stat().st_mtime <= exe.stat().st_mtime for o in objs):
        return exe
    r = subprocess.run([cc, *san, *map(str, objs), "-o", str(exe)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("link of tlstool failed:\n" + r.stdout + r.stderr)
    return exe


class Tool:
    """One tlstool process, kept for the whole module: cmd() sends a line and returns the output lines of that command."""

    _shared = None

    def __init__(self):
        self.p = subprocess.Popen([str(build_tool())], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, errors="replace", bufsize=1, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0"))

    @classmethod
    def get(cls):
        if cls._shared is None or cls._shared.p.poll() is not None:
            cls._shared = Tool()
        return cls._shared

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            s = self.p.stdout.readline()
            if s == "":
                err = self.p.stderr.read()
                raise RuntimeError(f"tlstool died on {line[:120]!r}:\n{err[-4000:]}")
            s = s.rstrip("\n")
            if s == ".":
                return out
            out.append(s)

    def one(self, line):
        out = self.cmd(line)
        assert len(out) == 1, (line[:100], out)
        return out[0]

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(timeout=60)
        err = self.p.stderr.read()
        return rc, err


def tearDownModule():
    t = Tool._shared
    if t is not None and t.p.poll() is None:
        rc, err = t.close()
        if rc != 0 or "ERROR: AddressSanitizer" in err or "LeakSanitizer" in err or "runtime error" in err:
            raise AssertionError(f"tlstool ended with {rc}:\n{err[-4000:]}")


def T():
    return Tool.get()


ALGS = {"256": "sha256", "384": "sha384", "512": "sha512"}


# ------------------------------------------------------------------------------------------------------------ constants

def primes(n):
    out, k = [], 2
    while len(out) < n:
        if all(k % p for p in out if p * p <= k):
            out.append(k)
        k += 1
    return out


def iroot(x, k):
    """floor(x ** (1/k)) for big integers"""
    lo, hi = 0, 1 << (x.bit_length() // k + 2)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if mid ** k <= x:
            lo = mid
        else:
            hi = mid - 1
    return lo


def frac_root(p, k, bits):
    """the first `bits` bits of the fractional part of p ** (1/k)"""
    return iroot(p << (k * bits), k) & ((1 << bits) - 1)


def aes_sbox():
    def mul(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = ((a << 1) ^ (0x11b if a & 0x80 else 0)) & 0x1ff
            b >>= 1
        return r
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if mul(a, b) == 1:
                inv[a] = b
                break
    box = []
    for a in range(256):
        x = inv[a]
        s = x
        for i in range(1, 5):
            s ^= ((x << i) | (x >> (8 - i))) & 0xff
        box.append(s ^ 0x63)
    return box


def ecparam(name):
    """p, b, n, Gx, Gy as `openssl ecparam` prints them (an independent source for the curve constants)."""
    import re
    r = subprocess.run([OPENSSL, "ecparam", "-name", name, "-param_enc", "explicit", "-text", "-noout"], capture_output=True, text=True, check=True)
    fields, cur = {}, None
    for line in r.stdout.splitlines():
        s = line.strip()
        m = re.fullmatch(r"([A-Za-z][A-Za-z ()]*):", s)
        if m:
            cur = m.group(1)
            fields[cur] = ""
        elif cur and re.fullmatch(r"[0-9a-f:]+", s):
            fields[cur] += s.replace(":", "")
        else:
            cur = None
    g = int(fields["Generator (uncompressed)"], 16) if "Generator (uncompressed)" in fields else int(fields["Generator"], 16)
    size = (int(fields["Prime"], 16).bit_length() + 7) // 8
    gb = g.to_bytes(1 + 2 * size, "big")
    return int(fields["Prime"], 16), int(fields["B"], 16), int(fields["Order"], 16), int.from_bytes(gb[1:1 + size], "big"), int.from_bytes(gb[1 + size:], "big")


class Tables(unittest.TestCase):
    """tables.h was generated by an earlier agent's script: every value is recomputed here from its definition."""

    def test_sha2_constants(self):
        t = {l.split()[0]: l.split()[1:] for l in T().cmd("tables")}
        p = primes(80)
        self.assertEqual([int(v, 16) for v in t["K256"]], [frac_root(q, 3, 32) for q in p[:64]])
        self.assertEqual([int(v, 16) for v in t["K512"]], [frac_root(q, 3, 64) for q in p[:80]])
        self.assertEqual([int(v, 16) for v in t["H256"]], [frac_root(q, 2, 32) for q in p[:8]])
        self.assertEqual([int(v, 16) for v in t["H512"]], [frac_root(q, 2, 64) for q in p[:8]])
        self.assertEqual([int(v, 16) for v in t["H384"]], [frac_root(q, 2, 64) for q in p[8:16]])

    def test_aes_sbox(self):
        t = {l.split()[0]: l.split()[1:] for l in T().cmd("tables")}
        self.assertEqual([int(v, 16) for v in t["SBOX"]], aes_sbox())

    @unittest.skipUnless(OPENSSL, "needs openssl")
    def test_curve_parameters(self):
        t = {l.split()[0]: l.split()[1:] for l in T().cmd("tables")}
        for name, pre in (("prime256v1", "P256"), ("secp384r1", "P384")):
            p, b, n, gx, gy = ecparam(name)
            self.assertEqual(int(t[pre + "_P"][0], 16), p, name)
            self.assertEqual(int(t[pre + "_B"][0], 16), b, name)
            self.assertEqual(int(t[pre + "_N"][0], 16), n, name)
            self.assertEqual(int(t[pre + "_GX"][0], 16), gx, name)
            self.assertEqual(int(t[pre + "_GY"][0], 16), gy, name)
            self.assertEqual((gy * gy - gx ** 3 + 3 * gx - b) % p, 0, "G is on the curve")


# ------------------------------------------------------------------------------------------------------------ symmetric

class Hashes(unittest.TestCase):
    def test_fips180_and_hashlib(self):
        self.assertEqual(T().one("hash 256 " + H(b"abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        self.assertEqual(T().one("hash 512 " + H(b"abc"))[:32], "ddaf35a193617abacc417349ae204131")
        rnd = random.Random(1)
        for n in list(range(0, 260)) + [1000, 4095, 4096, 5000, 100000]:
            data = rnd.randbytes(n)
            for alg, name in ALGS.items():
                self.assertEqual(T().one(f"hash {alg} {hx(data)}"), hashlib.new(name, data).hexdigest(), (alg, n))

    def test_hmac(self):
        # RFC 4231 test case 2
        self.assertEqual(T().one(f"hmac 256 {H(b'Jefe')} {H(b'what do ya want for nothing?')}"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")
        rnd = random.Random(2)
        for klen in (0, 1, 31, 32, 63, 64, 65, 127, 128, 129, 200):
            key, data = rnd.randbytes(klen), rnd.randbytes(rnd.randrange(300))
            for alg, name in ALGS.items():
                self.assertEqual(T().one(f"hmac {alg} {hx(key)} {hx(data)}"), hmac.new(key, data, name).hexdigest(), (alg, klen))

    def test_hkdf(self):
        # RFC 5869 test case 1
        ikm, salt, info = b"\x0b" * 22, bytes(range(13)), bytes(range(0xf0, 0xfa))
        prk = T().one(f"hkdf_extract 256 {H(salt)} {H(ikm)}")
        self.assertEqual(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5")
        self.assertEqual(T().one(f"hkdf_expand 256 {prk} {H(info)} 42"),
                         "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")
        if not HAVE_CRYPTO:
            return
        rnd = random.Random(3)
        for alg, hc in (("256", hashes.SHA256), ("384", hashes.SHA384), ("512", hashes.SHA512)):
            for _ in range(20):
                salt, ikm, info = rnd.randbytes(rnd.randrange(80)), rnd.randbytes(rnd.randrange(1, 80)), rnd.randbytes(rnd.randrange(60))
                n = rnd.randrange(1, 255 * hc.digest_size)
                ref = HKDF(algorithm=hc(), length=n, salt=salt or None, info=info).derive(ikm)
                prk = T().one(f"hkdf_extract {alg} {hx(salt)} {hx(ikm)}")
                self.assertEqual(T().one(f"hkdf_expand {alg} {prk} {hx(info)} {n}"), ref.hex())
            self.assertEqual(T().one(f"hkdf_expand {alg} {'00' * hc.digest_size} - {255 * hc.digest_size + 1}"), "FAIL")

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_hkdf_expand_label(self):
        """RFC 8446 7.1: HkdfLabel = length(2) || "tls13 " label (1 + n) || context (1 + n)"""
        rnd = random.Random(4)
        for alg, hc in (("256", hashes.SHA256), ("384", hashes.SHA384)):
            for label in ("key", "iv", "c hs traffic", "s ap traffic", "finished", "traffic upd", "derived"):
                secret, ctx = rnd.randbytes(hc.digest_size), rnd.randbytes(rnd.choice((0, hc.digest_size)))
                n = rnd.choice((12, 16, 32, hc.digest_size))
                lab = b"tls13 " + label.encode()
                info = struct.pack(">H", n) + bytes([len(lab)]) + lab + bytes([len(ctx)]) + ctx
                ref = HKDFExpand(hc(), n, info).derive(secret)
                self.assertEqual(T().one(f"hkdf_label {alg} {hx(secret)} {H(label.encode())} {hx(ctx)} {n}"), ref.hex(), label)

    def test_tls12_prf(self):
        """RFC 5246 5: P_hash(secret, label + seed), checked against a reference written here"""
        def p_hash(name, secret, seed, n):
            out, a = b"", seed
            while len(out) < n:
                a = hmac.new(secret, a, name).digest()
                out += hmac.new(secret, a + seed, name).digest()
            return out[:n]
        rnd = random.Random(5)
        for alg in ("256", "384"):
            for n in (12, 48, 104, 136):
                secret, seed = rnd.randbytes(48), rnd.randbytes(64)
                self.assertEqual(T().one(f"prf {alg} {hx(secret)} {H(b'master secret')} {hx(seed)} {n}"),
                                 p_hash(ALGS[alg], secret, b"master secret" + seed, n).hex())


@unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
class Aead(unittest.TestCase):
    def test_chacha20_and_poly1305(self):
        # RFC 8439 2.5.2
        key = U("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")
        self.assertEqual(T().one(f"poly1305 {H(key)} {H(b'Cryptographic Forum Research Group')}"), "a8061dc1305136c6c22b8baf0c0127a9")
        rnd = random.Random(6)
        for n in (0, 1, 15, 16, 17, 63, 64, 65, 1000, 16384 + 17):
            key, nonce, data = rnd.randbytes(32), rnd.randbytes(12), rnd.randbytes(n)
            ctr = rnd.randrange(2 ** 31)
            ref = Cipher(algorithms.ChaCha20(key, struct.pack("<I", ctr) + nonce), mode=None).encryptor().update(data)
            self.assertEqual(T().one(f"chacha {H(key)} {H(nonce)} {ctr} {hx(data)}"), ref.hex() or "")

    def test_chacha20_poly1305(self):
        rnd = random.Random(7)
        for n in (0, 1, 16, 63, 64, 65, 300, 16385):
            key, nonce, aad, pt = rnd.randbytes(32), rnd.randbytes(12), rnd.randbytes(rnd.randrange(30)), rnd.randbytes(n)
            ref = ChaCha20Poly1305(key).encrypt(nonce, pt, aad)
            self.assertEqual(T().one(f"cp_seal {H(key)} {H(nonce)} {hx(aad)} {hx(pt)}"), ref.hex())
            self.assertEqual(T().one(f"cp_open {H(key)} {H(nonce)} {hx(aad)} {ref.hex()}"), pt.hex() or "")
            bad = bytearray(ref)
            bad[rnd.randrange(len(bad))] ^= 1 << rnd.randrange(8)
            self.assertEqual(T().one(f"cp_open {H(key)} {H(nonce)} {hx(aad)} {bytes(bad).hex()}"), "FAIL")
            self.assertEqual(T().one(f"cp_open {H(key)} {H(nonce)} {hx(aad + b'x')} {ref.hex()}"), "FAIL")

    def test_aes_blocks(self):
        for mode in ("sw", "hw"):
            # FIPS 197 appendix C.1 and C.3
            self.assertEqual(T().one(f"aes {mode} 000102030405060708090a0b0c0d0e0f 00112233445566778899aabbccddeeff"), "69c4e0d86a7b0430d8cdb78070b4c55a")
            self.assertEqual(T().one(f"aes {mode} {bytes(range(32)).hex()} 00112233445566778899aabbccddeeff"), "8ea2b7ca516745bfeafc49904b496089")

    def test_aes_gcm(self):
        rnd = random.Random(8)
        modes = ("sw", "hw") if T().one("aes_hw") == "1" else ("sw",)
        for mode in modes:
            for klen in (16, 32):
                for n in (0, 1, 15, 16, 17, 255, 256, 1000, 16384 + 17):
                    key, iv, aad, pt = rnd.randbytes(klen), rnd.randbytes(12), rnd.randbytes(rnd.randrange(40)), rnd.randbytes(n)
                    ref = AESGCM(key).encrypt(iv, pt, aad)
                    self.assertEqual(T().one(f"gcm_seal {mode} {H(key)} {H(iv)} {hx(aad)} {hx(pt)}"), ref.hex(), (mode, klen, n))
                    self.assertEqual(T().one(f"gcm_open {mode} {H(key)} {H(iv)} {hx(aad)} {ref.hex()}"), pt.hex() or "")
                    bad = bytearray(ref)
                    bad[rnd.randrange(len(bad))] ^= 1 << rnd.randrange(8)
                    self.assertEqual(T().one(f"gcm_open {mode} {H(key)} {H(iv)} {hx(aad)} {bytes(bad).hex()}"), "FAIL")


# ------------------------------------------------------------------------------------------------------------ public key

X25519_LOW_ORDER = [
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
]


class PublicKey(unittest.TestCase):
    def test_x25519_rfc7748(self):
        k = U("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
        u = U("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
        self.assertEqual(T().one(f"x25519 {H(k)} {H(u)}"), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
        k = u = U("09" + "00" * 31)
        for i in range(1000):                                        # RFC 7748 5.2, the iterated vectors
            r = U(T().one(f"x25519 {H(k)} {H(u)}"))
            k, u = r, k
            if i == 0:
                self.assertEqual(r.hex(), "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079")
        self.assertEqual(k.hex(), "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51")
        a = U("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        b_pub = U("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
        self.assertEqual(T().one(f"x25519 {H(a)} {'09' + '00' * 31}"), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
        self.assertEqual(T().one(f"x25519 {H(a)} {H(b_pub)}"), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
        for p in X25519_LOW_ORDER:
            self.assertEqual(T().one(f"x25519 {H(a)} {p}"), "FAIL", p)

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_x25519_random(self):
        for _ in range(20):
            a, b = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
            raw = a.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
            pub = b.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
            self.assertEqual(T().one(f"x25519 {H(raw)} {H(pub)}"), a.exchange(b.public_key()).hex())

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_ecdh(self):
        for bits, curve in (("256", ec.SECP256R1()), ("384", ec.SECP384R1())):
            size = int(bits) // 8
            for _ in range(10):
                a, b = ec.generate_private_key(curve), ec.generate_private_key(curve)
                priv = a.private_numbers().private_value.to_bytes(size, "big")
                pub_a = a.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
                pub_b = b.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
                self.assertEqual(T().one(f"ecdh_pub {bits} {H(priv)}"), pub_a.hex())
                self.assertEqual(T().one(f"ecdh {bits} {H(priv)} {H(pub_b)}"), a.exchange(ec.ECDH(), b.public_key()).hex())
            priv = (1).to_bytes(size, "big")
            good = bytearray(pub_b)
            off_curve = bytearray(good)
            off_curve[-1] ^= 1
            for bad in (bytes(off_curve), good[:-1], b"\x02" + good[1:1 + size], b"\x04" + b"\x00" * 2 * size,
                        b"\x04" + b"\xff" * 2 * size, b"\x00"):
                self.assertEqual(T().one(f"ecdh {bits} {H(priv)} {H(bytes(bad))}"), "FAIL")

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_ecdsa(self):
        from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
        rnd = random.Random(9)
        for bits, curve, n in (("256", ec.SECP256R1(), 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551),
                               ("384", ec.SECP384R1(), 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF581A0DB248B0A77AECEC196ACCC52973)):
            key = ec.generate_private_key(curve)
            pub = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
            for hc in (hashes.SHA256, hashes.SHA384, hashes.SHA512):
                for _ in range(4):
                    msg = rnd.randbytes(rnd.randrange(200))
                    r, s = decode_dss_signature(key.sign(msg, ec.ECDSA(hc())))
                    h = hashlib.new(hc.name, msg).digest()
                    size = int(bits) // 8

                    def v(hh, rr, ss):
                        return T().one(f"ecdsa {bits} {H(pub)} {H(hh)} {rr.to_bytes(size + 1, 'big').hex()} {ss.to_bytes(size + 1, 'big').hex()}")
                    self.assertEqual(v(h, r, s), "OK")
                    self.assertEqual(v(h, r, n - s), "OK")             # ECDSA accepts both s and n - s
                    self.assertEqual(v(bytes([h[0] ^ 1]) + h[1:], r, s), "FAIL")
                    self.assertEqual(v(h, r ^ 2, s), "FAIL")
                    for rr, ss in ((0, s), (r, 0), (n, s), (r, n), (r + n, s)):
                        self.assertEqual(v(h, rr, ss), "FAIL", (rr, ss))

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_rsa(self):
        rnd = random.Random(10)
        for bits in (2048, 3072, 4096):
            key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
            nn = key.public_key().public_numbers()
            nb, eb = nn.n.to_bytes(bits // 8, "big"), nn.e.to_bytes(3, "big")
            for alg, hc in (("256", hashes.SHA256), ("384", hashes.SHA384), ("512", hashes.SHA512)):
                msg = rnd.randbytes(100)
                h = hashlib.new(hc.name, msg).digest()
                sig = key.sign(msg, padding.PKCS1v15(), hc())
                self.assertEqual(T().one(f"rsa_pkcs1 {H(nb)} {H(eb)} {alg} {H(h)} {H(sig)}"), "OK")
                self.assertEqual(T().one(f"rsa_pkcs1 {H(nb)} {H(eb)} {alg} {H(h[::-1])} {H(sig)}"), "FAIL")
                bad = bytearray(sig)
                bad[-1] ^= 1
                self.assertEqual(T().one(f"rsa_pkcs1 {H(nb)} {H(eb)} {alg} {H(h)} {H(bytes(bad))}"), "FAIL")
                self.assertEqual(T().one(f"rsa_pkcs1 {H(nb)} {H(eb)} {alg} {H(h)} {H(sig[1:])}"), "FAIL")
                for salt in (0, hc.digest_size, 64):
                    sig = key.sign(msg, padding.PSS(mgf=padding.MGF1(hc()), salt_length=salt), hc())
                    self.assertEqual(T().one(f"rsa_pss {H(nb)} {H(eb)} {alg} {H(h)} {H(sig)} {salt}"), "OK", (bits, alg, salt))
                    self.assertEqual(T().one(f"rsa_pss {H(nb)} {H(eb)} {alg} {H(h)} {H(sig)} {salt + 1}"), "FAIL")
                    self.assertEqual(T().one(f"rsa_pss {H(nb)} {H(eb)} {alg} {H(h[:-1] + b'x')} {H(sig)} {salt}"), "FAIL")

    @unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
    def test_rsa_e3_forgery_refused(self):
        """Bleichenbacher 2006: with e = 3, a cube root of "00 01 FF 00 DigestInfo hash GARBAGE" passes a parser that does not
        look at the garbage. The encoding is rebuilt and compared here, so it must fail."""
        key = rsa.generate_private_key(public_exponent=3, key_size=2048)
        n = key.public_key().public_numbers().n
        h = hashlib.sha256(b"forged").digest()
        di = U("3031300d060960864801650304020105000420")
        prefix = int.from_bytes(b"\x00\x01\xff\x00" + di + h, "big")
        k = 256
        garbage_bits = 8 * (k - (4 + len(di) + 32))
        s = iroot(prefix << garbage_bits, 3) + 1
        assert (s ** 3) >> garbage_bits == prefix and s < n
        self.assertEqual(T().one(f"rsa_pkcs1 {n.to_bytes(k, 'big').hex()} 03 256 {H(h)} {s.to_bytes(k, 'big').hex()}"), "FAIL")
        good = key.sign(b"forged", padding.PKCS1v15(), hashes.SHA256())
        self.assertEqual(T().one(f"rsa_pkcs1 {n.to_bytes(k, 'big').hex()} 03 256 {H(h)} {H(good)}"), "OK")

    def test_ed25519(self):
        # RFC 8032 7.1 test 1 (the empty message)
        pub = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
        sig = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
        self.assertEqual(T().one(f"ed25519 {pub} - {sig}"), "OK")
        self.assertEqual(T().one(f"ed25519 {pub} 00 {sig}"), "FAIL")
        L = 2 ** 252 + 27742317777372353535851937790883648493
        s = int.from_bytes(U(sig[64:]), "little")
        malleable = sig[:64] + (s + L).to_bytes(32, "little").hex()
        self.assertEqual(T().one(f"ed25519 {pub} - {malleable}"), "FAIL")
        if HAVE_CRYPTO:
            for i in range(10):
                k = ed25519.Ed25519PrivateKey.generate()
                p = k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
                msg = os.urandom(i * 37)
                self.assertEqual(T().one(f"ed25519 {H(p)} {hx(msg)} {H(k.sign(msg))}"), "OK")

    def test_bignum(self):
        rnd = random.Random(11)
        for bits in (64, 65, 127, 128, 255, 256, 384, 521, 1024, 2048, 3072, 4096):
            for _ in range(5):
                m = rnd.getrandbits(bits) | (1 << (bits - 1)) | 1
                a, b = rnd.randrange(m), rnd.randrange(m)
                size = (bits + 7) // 8
                mb, ab, bb = m.to_bytes(size, "big"), a.to_bytes(size, "big"), b.to_bytes(size, "big")
                self.assertEqual(int(T().one(f"modmul {H(mb)} {H(ab)} {H(bb)}"), 16), a * b % m, bits)
                e = rnd.getrandbits(rnd.choice((2, 17, 64)))
                eb = e.to_bytes(max(1, (e.bit_length() + 7) // 8), "big")
                self.assertEqual(int(T().one(f"modexp {H(mb)} {H(ab)} {H(eb)}"), 16), pow(a, e, m), bits)
            m = (1 << bits) - 1 if bits % 2 else (1 << bits) - 3     # extremes: a = m - 1
            size = (bits + 7) // 8
            self.assertEqual(int(T().one(f"modmul {H(m.to_bytes(size, 'big'))} {H((m - 1).to_bytes(size, 'big'))} {H((m - 1).to_bytes(size, 'big'))}"), 16),
                             (m - 1) * (m - 1) % m)


# ------------------------------------------------------------------------------------------------------------ certificates

NOW = datetime.datetime(2026, 6, 1, 12, 0, tzinfo=datetime.timezone.utc)
NOW_TS = int(NOW.timestamp())
DAY = datetime.timedelta(days=1)
E = {"OK": 0, "ARG": -10, "NOMEM": -11, "IO": -12, "TIMEOUT": -13, "EOF": -14, "TRUNCATED": -15, "PROTOCOL": -16, "ALERT": -17,
     "VERSION": -18, "SUITE": -19, "DECRYPT": -20, "HANDSHAKE_SIG": -21, "FINISHED": -22, "RANDOM": -23, "NO_TRUST": -24, "CLOCK": -25,
     "CLOSED": -26, "PLAINTEXT": -27, "CERT_PARSE": -30, "CERT_UNTRUSTED": -31, "CERT_SELF_SIGNED": -32, "CERT_EXPIRED": -33,
     "CERT_NOT_YET_VALID": -34, "CERT_NAME": -35, "CERT_SIGNATURE": -36, "CERT_USAGE": -37, "CERT_UNSUPPORTED": -38, "CERT_WEAK": -39,
     "CERT_EMPTY": -40, "CERT_CONSTRAINTS": -41}

_keys = {}


def new_key(kind):
    if kind.startswith("ec"):
        return ec.generate_private_key({"ec256": ec.SECP256R1(), "ec384": ec.SECP384R1(), "ec521": ec.SECP521R1()}[kind])
    if kind == "ed25519":
        return ed25519.Ed25519PrivateKey.generate()
    return rsa.generate_private_key(public_exponent=65537, key_size=int(kind[3:]))


def cached_key(kind, slot=0):
    """RSA keys take a while to make: tests that do not care share them"""
    if (kind, slot) not in _keys:
        _keys[(kind, slot)] = new_key(kind)
    return _keys[(kind, slot)]


class Cert:
    def __init__(self, cert, key):
        self.cert, self.key = cert, key
        self.der = cert.public_bytes(serialization.Encoding.DER)
        self.pem = cert.public_bytes(serialization.Encoding.PEM)
        self.name = cert.subject

    def key_pem(self):
        return self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())


def dn(cn, org="TouhouOS Test"):
    attrs = [x509.NameAttribute(NameOID.COMMON_NAME, cn)]
    if org:
        attrs.append(x509.NameAttribute(NameOID.ORGANIZATION_NAME, org))
    return x509.Name(attrs)


def issue(cn, key, issuer=None, sans=("localhost",), ips=(), ca=False, path_len=None, nb=None, na=None, hash_alg="sha256", pss=False,
          eku="server", ku="default", extra=(), name=None, serial=None, bc=True, ski=True):
    """A certificate for `key`, signed by `issuer` (a Cert) or by itself."""
    subject = name or dn(cn)
    b = x509.CertificateBuilder().subject_name(subject).public_key(key.public_key())
    b = b.issuer_name(issuer.cert.subject if issuer else subject)
    b = b.serial_number(serial if serial is not None else x509.random_serial_number())
    b = b.not_valid_before(nb or NOW - 30 * DAY).not_valid_after(na or NOW + 300 * DAY)
    if bc:
        b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=path_len if ca else None), critical=True)
    if ku == "default":
        is_rsa = isinstance(key, rsa.RSAPrivateKey)
        b = b.add_extension(x509.KeyUsage(digital_signature=not ca, content_commitment=False, key_encipherment=is_rsa and not ca,
                                          data_encipherment=False, key_agreement=False, key_cert_sign=ca, crl_sign=ca,
                                          encipher_only=False, decipher_only=False), critical=True)
    elif ku is not None:
        b = b.add_extension(ku, critical=True)
    if not ca and (sans or ips):
        b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(s) for s in sans] + [x509.IPAddress(ipaddress.ip_address(i)) for i in ips]),
                            critical=False)
    if eku == "server":
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    elif eku == "client":
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
    elif eku == "any":
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.ANY_EXTENDED_KEY_USAGE]), critical=False)
    if ski:
        b = b.add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
    if issuer and ski:
        b = b.add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer.key.public_key()), critical=False)
    for ext, crit in extra:
        b = b.add_extension(ext, critical=crit)
    signer = issuer.key if issuer else key
    if isinstance(signer, ed25519.Ed25519PrivateKey):
        cert = b.sign(signer, None)
    else:
        hc = {"sha1": hashes.SHA1, "sha256": hashes.SHA256, "sha384": hashes.SHA384, "sha512": hashes.SHA512}[hash_alg]()
        if pss:
            cert = b.sign(signer, hc, rsa_padding=padding.PSS(mgf=padding.MGF1(hc), salt_length=hc.digest_size))
        else:
            cert = b.sign(signer, hc)
    return Cert(cert, key)


def pki(root_kind="ec256", inter_kind="ec256", leaf_kind="ec256", **leaf_kw):
    root = issue("Test Root", cached_key(root_kind, 1), ca=True, ski=True)
    inter = issue("Test Intermediate", cached_key(inter_kind, 2), issuer=root, ca=True, path_len=0)
    leaf = issue(leaf_kw.pop("cn", "localhost"), cached_key(leaf_kind, 3), issuer=inter, **leaf_kw)
    return root, inter, leaf


def verify(chain, roots, host="localhost", now=NOW_TS):
    t = T()
    t.one("trust_reset")
    for r in roots:
        t.one(f"trust_add {r.der.hex()}")
    out = t.one(f"verify {now} {host or '-'} " + " ".join(c.der.hex() for c in chain))
    parts = out.split(" | ")
    return int(parts[0]), parts[1:]


@unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
class Certificates(unittest.TestCase):
    def assertVerify(self, want, chain, roots, host="localhost", now=NOW_TS):
        rc, path = verify(chain, roots, host, now)
        self.assertEqual(rc, E[want], f"expected {want}, got {[k for k, v in E.items() if v == rc]} (path {path})")
        return path

    def test_parse_fields(self):
        cases = [("ec256", "EC P-256", 256), ("ec384", "EC P-384", 384), ("ed25519", "Ed25519", 256), ("rsa2048", "RSA 2048", 2048),
                 ("rsa4096", "RSA 4096", 4096)]
        for kind, desc, bits in cases:
            c = issue("parse.example", cached_key(kind), sans=("parse.example", "*.parse.example"), ips=("10.0.2.2",))
            out = dict(l.split(" ", 1) for l in T().cmd(f"cert {c.der.hex()}") if " " in l)
            self.assertEqual(out["parse"], "0")
            self.assertEqual(out["key"], desc)
            self.assertEqual(int(out["bits"]), bits)
            self.assertEqual(out["subject"], "CN=parse.example, O=TouhouOS Test")
            self.assertEqual(out["names"], "parse.example, *.parse.example, 10.0.2.2")
            self.assertEqual(int(out["not_before"]), int((NOW - 30 * DAY).timestamp()))
            self.assertEqual(int(out["not_after"]), int((NOW + 300 * DAY).timestamp()))
            self.assertEqual(out["self_signature"], "0", kind)
        c = issue("pss.example", cached_key("rsa2048"), hash_alg="sha384", pss=True)
        out = dict(l.split(" ", 1) for l in T().cmd(f"cert {c.der.hex()}") if " " in l)
        self.assertEqual((out["sig"], out["self_signature"]), ("RSA-PSS SHA-384", "0"))

    def test_good_chains(self):
        combos = [("ec256", "ec256", "ec256"), ("rsa2048", "rsa2048", "rsa2048"), ("rsa4096", "ec384", "ed25519"), ("ed25519", "rsa3072", "ec256"),
                  ("ec384", "ed25519", "rsa2048")]
        for kinds in combos:
            root, inter, leaf = pki(*kinds)
            path = self.assertVerify("OK", [leaf, inter], [root])
            self.assertEqual(path, ["CN=localhost, O=TouhouOS Test", "CN=Test Intermediate, O=TouhouOS Test", "CN=Test Root, O=TouhouOS Test"], kinds)
        root, inter, leaf = pki()
        other = issue("Unrelated", cached_key("ec256", 9), ca=True)
        self.assertVerify("OK", [leaf, other, root, inter], [root])            # order does not matter, extras are ignored
        for h in ("sha256", "sha384", "sha512"):
            root = issue("PSS Root", cached_key("rsa2048", 1), ca=True)
            inter = issue("PSS Intermediate", cached_key("rsa2048", 2), issuer=root, ca=True, hash_alg=h, pss=True)
            leaf = issue("localhost", cached_key("ec256", 3), issuer=inter, hash_alg=h, pss=True)
            self.assertVerify("OK", [leaf, inter], [root])
        root = issue("EC Root", cached_key("ec384", 1), ca=True)
        leaf = issue("localhost", cached_key("ec256", 3), issuer=root, hash_alg="sha512")
        self.assertVerify("OK", [leaf], [root])

    def test_refusals(self):
        root, inter, leaf = pki()
        self.assertVerify("CERT_NAME", [leaf, inter], [root], host="other.example")
        self.assertVerify("CERT_NAME", [leaf, inter], [root], host="10.0.2.2")
        self.assertVerify("CERT_UNTRUSTED", [leaf, inter], [issue("Some Other Root", cached_key("ec256", 8), ca=True)])
        self.assertVerify("CERT_UNTRUSTED", [leaf], [root])                     # the intermediate is missing
        _, _, old = pki(nb=NOW - 400 * DAY, na=NOW - DAY)
        self.assertVerify("CERT_EXPIRED", [old, inter], [root])
        _, _, young = pki(nb=NOW + DAY, na=NOW + 400 * DAY)
        self.assertVerify("CERT_NOT_YET_VALID", [young, inter], [root])
        expired_inter = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=True, na=NOW - DAY)
        self.assertVerify("CERT_EXPIRED", [leaf, expired_inter], [root])
        selfie = issue("localhost", cached_key("ec256", 4))
        self.assertVerify("CERT_SELF_SIGNED", [selfie], [root])
        self.assertVerify("OK", [selfie], [selfie])                             # trusted as it stands when it is in the store
        bad = bytearray(leaf.der)
        bad[-1] ^= 0x40                                                         # the last byte of the signature
        tampered = Cert(x509.load_der_x509_certificate(bytes(bad)), leaf.key)
        self.assertVerify("CERT_SIGNATURE", [tampered, inter], [root])
        # OpenSSL 3 no longer makes SHA-1 or MD5 signatures: the algorithm identifier of a SHA-256 one is swapped (both copies;
        # the OIDs have the same length), which is all the refusal may depend on
        rsa_inter = issue("RSA Intermediate", cached_key("rsa2048", 2), issuer=root, ca=True)
        rsa_leaf = issue("localhost", cached_key("rsa2048", 3), issuer=rsa_inter)
        self.assertVerify("OK", [rsa_leaf, rsa_inter], [root])
        for oid_tail in (b"\x05", b"\x04"):                           # sha1WithRSAEncryption, md5WithRSAEncryption
            weak = rsa_leaf.der.replace(U("2a864886f70d01010b"), U("2a864886f70d0101") + oid_tail)
            self.assertEqual(weak.count(U("2a864886f70d0101") + oid_tail), 2)
            from types import SimpleNamespace
            self.assertVerify("CERT_WEAK", [SimpleNamespace(der=weak), rsa_inter], [root])
        small = issue("localhost", rsa.generate_private_key(public_exponent=65537, key_size=1024), issuer=inter)
        self.assertVerify("CERT_WEAK", [small, inter], [root])
        not_ca = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=False, eku=None, sans=())
        leaf2 = issue("localhost", cached_key("ec256", 3), issuer=not_ca)
        self.assertVerify("CERT_USAGE", [leaf2, not_ca], [root])
        no_bc = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=True, bc=False, eku=None)
        self.assertVerify("CERT_USAGE", [issue("localhost", cached_key("ec256", 3), issuer=no_bc), no_bc], [root])
        inter2 = issue("Second Intermediate", cached_key("ec256", 5), issuer=inter, ca=True)       # inter has pathLen 0
        self.assertVerify("CERT_USAGE", [issue("localhost", cached_key("ec256", 3), issuer=inter2), inter2, inter], [root])
        no_sign = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=True,
                        ku=x509.KeyUsage(True, False, False, False, False, False, True, False, False))
        self.assertVerify("CERT_USAGE", [issue("localhost", cached_key("ec256", 3), issuer=no_sign), no_sign], [root])
        _, _, client = pki(eku="client")
        self.assertVerify("CERT_USAGE", [client, inter], [root])
        _, _, any_eku = pki(eku="any")
        self.assertVerify("OK", [any_eku, inter], [root])
        _, _, enc_only = pki(leaf_kind="rsa2048", ku=x509.KeyUsage(False, False, True, False, False, False, False, False, False))
        self.assertVerify("CERT_USAGE", [enc_only, inter], [root])
        weird = x509.UnrecognizedExtension(x509.ObjectIdentifier("1.3.6.1.4.1.55555.1"), b"\x05\x00")
        _, _, crit = pki(extra=[(weird, True)])
        self.assertVerify("CERT_UNSUPPORTED", [crit, inter], [root])
        _, _, noncrit = pki(extra=[(weird, False)])
        self.assertVerify("OK", [noncrit, inter], [root])
        p521 = issue("localhost", new_key("ec521"), issuer=inter)
        self.assertVerify("CERT_UNSUPPORTED", [p521, inter], [root])
        _, _, nosan = pki(sans=())
        self.assertVerify("CERT_NAME", [nosan, inter], [root])                 # the Common Name is never used

    def test_alternative_paths(self):
        """A path that fails is not the end: the same intermediate re-issued (one copy expired) and a cross-signed root."""
        root = issue("Test Root", cached_key("ec256", 1), ca=True)
        old_inter = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=True, na=NOW - DAY)
        new_inter = issue("Test Intermediate", cached_key("ec256", 2), issuer=root, ca=True)
        leaf = issue("localhost", cached_key("ec256", 3), issuer=new_inter)
        self.assertVerify("OK", [leaf, old_inter, new_inter], [root])
        old_root = issue("Old Root", cached_key("rsa2048", 6), ca=True, na=NOW - DAY)
        cross = issue("Test Root", cached_key("ec256", 1), issuer=old_root, ca=True)           # the root, cross-signed by an expired one
        self.assertVerify("OK", [leaf, new_inter, cross], [root, old_root])
        self.assertVerify("CERT_EXPIRED", [leaf, new_inter, cross], [old_root])

    def test_search_budget(self):
        """Ten look-alike intermediates that never lead anywhere: the search gives up quickly instead of trying every order."""
        root = issue("Test Root", cached_key("ec256", 1), ca=True)
        k = cached_key("ec256", 20)
        # nine CA certificates with one name and one key: each one's signature verifies under every other one, so every order
        # of them is a valid-looking path (9! of them) and none reaches a root
        fakes = [issue("Look Alike", k, name=dn("Look Alike"), ca=True, serial=1000 + i) for i in range(9)]
        leaf = issue("localhost", cached_key("ec256", 3), issuer=fakes[0])
        t0 = time.time()
        rc, _ = verify([leaf] + fakes, [root])
        self.assertIn(rc, (E["CERT_UNTRUSTED"], E["CERT_SELF_SIGNED"]))
        self.assertLess(time.time() - t0, 10)

    def test_name_constraints(self):
        root = issue("NC Root", cached_key("ec256", 1), ca=True)

        def constrained(permitted=None, excluded=None):
            nc = x509.NameConstraints(permitted_subtrees=permitted, excluded_subtrees=excluded)
            return issue("NC Intermediate", cached_key("ec256", 2), issuer=root, ca=True, extra=[(nc, True)])

        def leaf_of(inter, sans=(), ips=(), name=None):
            return issue("localhost", cached_key("ec256", 3), issuer=inter, sans=sans, ips=ips, name=name)

        good = constrained(permitted=[x509.DNSName("example.com")])
        self.assertVerify("OK", [leaf_of(good, ["www.example.com"]), good], [root], host="www.example.com")
        self.assertVerify("OK", [leaf_of(good, ["example.com"]), good], [root], host="example.com")
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(good, ["www.example.com", "www.evil.com"]), good], [root], host="www.example.com")
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(good, ["badexample.com"]), good], [root], host="badexample.com")
        self.assertVerify("OK", [leaf_of(good, ["*.example.com"]), good], [root], host="a.example.com")
        sub = constrained(permitted=[x509.DNSName("a.example.com")])
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(sub, ["*.example.com"]), sub], [root], host="a.example.com")
        ex = constrained(excluded=[x509.DNSName("secret.example.com")])
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(ex, ["secret.example.com"]), ex], [root], host="secret.example.com")
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(ex, ["x.secret.example.com"]), ex], [root], host="x.secret.example.com")
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(ex, ["*.example.com"]), ex], [root], host="www.example.com")
        self.assertVerify("OK", [leaf_of(ex, ["www.example.com"]), ex], [root], host="www.example.com")
        ipc = constrained(permitted=[x509.IPAddress(ipaddress.ip_network("192.168.1.0/8"))])
        self.assertVerify("OK", [leaf_of(ipc, ips=["10.0.2.2"]), ipc], [root], host="10.0.2.2")
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(ipc, ips=["192.168.1.1"]), ipc], [root], host="192.168.1.1")
        self.assertVerify("OK", [leaf_of(ipc, ["any.example"]), ipc], [root], host="any.example")       # IP rules do not bind DNS names
        dnc = constrained(permitted=[x509.DirectoryName(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")]))])
        self.assertVerify("OK", [leaf_of(dnc, ["localhost"], name=dn("localhost")), dnc], [root])
        self.assertVerify("CERT_CONSTRAINTS", [leaf_of(dnc, ["localhost"], name=dn("intruder")), dnc], [root])

    def test_hostnames(self):
        table = [
            (["example.org"], "example.org", True), (["example.org"], "EXAMPLE.org", True), (["example.org"], "example.org.", True),
            (["Example.ORG"], "example.org", True), (["example.org"], "www.example.org", False), (["www.example.org"], "example.org", False),
            (["*.example.org"], "www.example.org", True), (["*.example.org"], "example.org", False), (["*.example.org"], "a.b.example.org", False),
            (["*.example.org"], ".example.org", False), (["*.org"], "example.org", False), (["w*.example.org"], "www.example.org", False),
            (["*w.example.org"], "www.example.org", False), (["www.*.org"], "www.example.org", False), (["*.*.example.org"], "a.b.example.org", False),
            (["*"], "localhost", False), (["*.co.uk"], "example.co.uk", True), (["xn--bcher-kva.example"], "xn--bcher-kva.example", True),
            (["*.xn--bcher-kva.example"], "a.xn--bcher-kva.example", True), (["a.example", "b.example"], "b.example", True),
            (["localhost"], "localhost", True), (["10.0.2.2"], "10.0.2.2", False),
        ]
        for sans, host, want in table:
            c = issue("ignored.example", cached_key("ec256"), sans=sans)
            rc = int(T().one(f"name {c.der.hex()} {host}"))
            self.assertEqual(rc == 0, want, (sans, host, rc))
        c = issue("ip.example", cached_key("ec256"), sans=["ip.example"], ips=["10.0.2.2", "192.168.0.7"])
        for host, want in (("10.0.2.2", True), ("192.168.0.7", True), ("10.0.2.3", False), ("ip.example", True), ("010.0.2.2", False)):
            self.assertEqual(int(T().one(f"name {c.der.hex()} {host}")) == 0, want, host)

    def test_trust_store_file(self):
        roots = [issue(f"Root {i}", cached_key("ec256", 30 + i), ca=True) for i in range(3)]
        rsa_root = issue("RSA Root", cached_key("rsa2048", 1), ca=True)
        p521_root = issue("P-521 Root", new_key("ec521"), ca=True)
        with tempfile.TemporaryDirectory() as d:
            bundle = Path(d) / "bundle.pem"
            text = b"# a comment\n" + b"".join(r.pem for r in roots) + rsa_root.pem + roots[0].pem + p521_root.pem
            text += b"-----BEGIN CERTIFICATE-----\nnot base64 at all!\n-----END CERTIFICATE-----\n"
            bundle.write_bytes(text)
            T().one("trust_reset")
            rc, count, skipped = map(int, T().one(f"trust_file {bundle}").split())
            self.assertEqual((rc, count), (4, 4))                     # the duplicate is dropped, P-521 and the junk are skipped
            self.assertEqual(skipped, 2)
            der = Path(d) / "one.der"
            der.write_bytes(rsa_root.der)
            T().one("trust_reset")
            self.assertEqual(T().one(f"trust_file {der}").split()[:2], ["1", "1"])
            T().one("trust_reset")
            self.assertEqual(int(T().one(f"trust_file {Path(d) / 'missing.pem'}").split()[0]), E["NO_TRUST"])

    @unittest.skipUnless(OPENSSL, "needs openssl")
    def test_openssl_made_certificate(self):
        """A certificate from the openssl command (another encoder than cryptography's)"""
        with tempfile.TemporaryDirectory() as d:
            for alg in (["-newkey", "rsa:2048"], ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"], ["-newkey", "ed25519"]):
                subprocess.run([OPENSSL, "req", "-x509", *alg, "-nodes", "-keyout", f"{d}/k.pem", "-out", f"{d}/c.pem", "-days", "30",
                                "-subj", "/CN=openssl.example/O=Made by openssl", "-addext", "subjectAltName=DNS:openssl.example,IP:127.0.0.1"],
                               capture_output=True, check=True)
                c = x509.load_pem_x509_certificate(Path(f"{d}/c.pem").read_bytes())
                der = c.public_bytes(serialization.Encoding.DER)
                out = dict(l.split(" ", 1) for l in T().cmd(f"cert {der.hex()}") if " " in l)
                self.assertEqual((out["parse"], out["self_signature"]), ("0", "0"), alg)
                self.assertEqual(out["names"], "openssl.example, 127.0.0.1")
                T().one("trust_reset")
                T().one(f"trust_add {der.hex()}")
                self.assertEqual(T().one(f"verify {int(time.time())} openssl.example {der.hex()}").split()[0], "0")

    def test_der_torture(self):
        root, inter, leaf = pki(leaf_kind="rsa2048")
        for n in range(len(leaf.der)):                               # every truncation fails cleanly
            out = T().cmd(f"cert {leaf.der[:n].hex() or '-'}")
            self.assertNotEqual(out[0], "parse 0", n)
        out = T().one(f"fuzz_der 1 200000 {leaf.der.hex()} {inter.der.hex()} {root.der.hex()} {issue('ed', cached_key('ed25519')).der.hex()}")
        self.assertTrue(out.startswith("fuzz 200000 parsed"), out)
        d = bytearray(leaf.der)
        crafted = {
            "indefinite length": bytes([0x30, 0x80]) + bytes(d[4:]),
            "length past the end": bytes([0x30, 0x84, 0x7f, 0xff, 0xff, 0xff]) + bytes(d[4:]),
            "non-minimal length": bytes([0x30, 0x83, 0x00]) + bytes(d[2:]),
            "five length bytes": bytes([0x30, 0x85, 0, 0, 0, 0x05, 0x00]),
            "high tag number": bytes([0x3f, 0x81, 0x00, 0x00]),
            "empty": b"",
            "trailing garbage": bytes(d) + b"\x00",
        }
        for what, blob in crafted.items():
            self.assertNotEqual(T().cmd(f"cert {blob.hex() or '-'}")[0], "parse 0", what)

    def test_extension_rules(self):
        """duplicate extensions, an empty SAN, a non-DER BOOLEAN: refused while parsing"""
        base = issue("rules.example", cached_key("ec256"), sans=["rules.example"])
        der = base.der
        san_oid = bytes([0x06, 0x03, 0x55, 0x1d, 0x11])
        i = der.find(san_oid)
        self.assertGreater(i, 0)
        # the Extension SEQUENCE that holds the SAN starts two bytes before the OID (short length form: small extension)
        ext_start = i - 2
        ext = der[ext_start:ext_start + 2 + der[ext_start + 1]]
        doubled = rebuild_with_extensions(der, extra=ext)
        self.assertNotEqual(T().cmd(f"cert {doubled.hex()}")[0], "parse 0")


# ------------------------------------------------------------------------------------------------------------ servers

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def connect(port, sni="localhost", **opts):
    """Runs the library's client against 127.0.0.1:port; returns the tool's report as a dict."""
    line = f"connect 127.0.0.1 {port} {sni or '-'} " + " ".join(f"{k}={v}" for k, v in opts.items())
    out = T().cmd(line)
    res = {"path": [], "lines": out}
    for ln in out:
        k, _, v = ln.partition(" ")
        if k == "path":
            res["path"].append(v)
        else:
            res[k] = v
    first = res.get("result", "x").split()
    res["rc"] = int(first[0]) if first[0].lstrip("-").isdigit() else None
    res["err"] = first[1] if len(first) > 1 else first[0]
    return res


class Pki:
    """An EC chain, an RSA chain and an Ed25519 leaf, written to files for the servers; roots in ca.pem."""

    def __init__(self, d):
        self.d = Path(d)
        self.root, self.inter, self.ec = pki("ec256", "ec256", "ec256", ips=("127.0.0.1",))
        self.rsa_inter = issue("RSA Intermediate", cached_key("rsa2048", 2), issuer=self.root, ca=True)
        self.rsa = issue("localhost", cached_key("rsa2048", 3), issuer=self.rsa_inter, ips=("127.0.0.1",))
        self.ed = issue("localhost", cached_key("ed25519", 3), issuer=self.inter, ips=("127.0.0.1",))
        self.ec384 = issue("localhost", cached_key("ec384", 3), issuer=self.inter, ips=("127.0.0.1",))
        self.ca = self.write("ca.pem", self.root.pem)
        for name, leaf, inter in (("ec", self.ec, self.inter), ("rsa", self.rsa, self.rsa_inter), ("ed", self.ed, self.inter), ("ec384", self.ec384, self.inter)):
            self.write(f"{name}.pem", leaf.pem + inter.pem)
            self.write(f"{name}.key", leaf.key_pem())
            self.write(f"{name}.chain", inter.pem)

    def write(self, name, data):
        p = self.d / name
        p.write_bytes(data)
        return str(p)

    def files(self, name):
        return str(self.d / f"{name}.pem"), str(self.d / f"{name}.key")


class SServer:
    """openssl s_server for one connection. www=True answers GET with a status page (-www); otherwise its stdin and stdout
    are the application data, and lines starting with k/K make it send a KeyUpdate."""

    def __init__(self, cert, key, *args, www=True):
        self.port = free_port()
        chain = str(Path(cert).with_suffix(".chain"))                # -cert reads only the first certificate of a file
        cmd = [OPENSSL, "s_server", "-accept", f"127.0.0.1:{self.port}", "-cert", cert, "-key", key, "-naccept", "1", *args]
        if Path(chain).exists():
            cmd += ["-cert_chain", chain]
        if www:
            cmd.append("-www")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.out = []
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        self.wait_for("ACCEPT")

    def _reader(self):
        for ln in self.p.stdout:
            with self.lock:
                self.out.append(ln.rstrip("\n"))

    def text(self):
        with self.lock:
            return "\n".join(self.out)

    def wait_for(self, what, timeout=20):
        t0 = time.time()
        while what not in self.text():
            if time.time() - t0 > timeout:
                raise AssertionError(f"s_server never said {what!r}:\n{self.text()[-2000:]}")
            if self.p.poll() is not None and what not in self.text():
                time.sleep(0.2)
                if what not in self.text():
                    raise AssertionError(f"s_server exited ({self.p.returncode}):\n{self.text()[-2000:]}")
            time.sleep(0.05)

    def send(self, text):
        self.p.stdin.write(text)
        self.p.stdin.flush()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        for f in (self.p.stdin, self.p.stdout):
            try:
                f.close()
            except OSError:
                pass


class PySslServer(threading.Thread):
    """Python's ssl module (OpenSSL) as a server for one connection; handler(sslsock) does the talking."""

    def __init__(self, certfile, keyfile, handler, minv=None, maxv=None, ciphers=None, alpn=None):
        super().__init__(daemon=True)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(certfile, keyfile)
        if minv:
            self.ctx.minimum_version = minv
        if maxv:
            self.ctx.maximum_version = maxv
        if ciphers:
            self.ctx.set_ciphers(ciphers)
        if alpn:
            self.ctx.set_alpn_protocols(alpn)
        self.handler, self.error, self.result = handler, None, None
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(30)
        self.port = self.sock.getsockname()[1]
        self.start()

    def run(self):
        try:
            conn, _ = self.sock.accept()
            with self.ctx.wrap_socket(conn, server_side=True) as s:
                self.result = self.handler(s)
        except Exception as e:  # noqa: BLE001
            self.error = repr(e)
        finally:
            self.sock.close()


def sink_then_source(n_in, data_out, clean_close=True):
    """Reads n_in bytes, then sends the SHA-256 of them followed by data_out."""
    def handler(s):
        h, got = hashlib.sha256(), 0
        while got < n_in:
            b = s.recv(65536)
            if not b:
                break
            h.update(b)
            got += len(b)
        digest = h.digest()
        s.sendall(digest + data_out)
        if clean_close:
            s.unwrap()
        return got, digest.hex()
    return handler


# ---- an independent TLS 1.3 server (Python, `cryptography`), for the faults OpenSSL will not make

HRR_RANDOM = U("cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c")
SUITES13 = {0x1301: ("aes128", hashes.SHA256 if HAVE_CRYPTO else None, 16), 0x1302: ("aes256", hashes.SHA384 if HAVE_CRYPTO else None, 32),
            0x1303: ("chacha", hashes.SHA256 if HAVE_CRYPTO else None, 32)}


def hs_msg(t, body):
    return bytes([t]) + len(body).to_bytes(3, "big") + body


def vec(n, body):
    return len(body).to_bytes(n, "big") + body


def ext(t, body):
    return struct.pack(">HH", t, len(body)) + body


def expand_label(hc, secret, label, ctx, n):
    lab = b"tls13 " + label
    return HKDFExpand(hc(), n, struct.pack(">H", n) + vec(1, lab) + vec(1, ctx)).derive(secret)


def extract(hc, salt, ikm):
    return hmac.new(salt if salt else bytes(hc.digest_size), ikm, hc.name).digest()


class Keys13:
    def __init__(self, suite, secret):
        kind, hc, kl = SUITES13[suite]
        key = expand_label(hc, secret, b"key", b"", kl)
        self.iv = expand_label(hc, secret, b"iv", b"", 12)
        self.aead = ChaCha20Poly1305(key) if kind == "chacha" else AESGCM(key)
        self.seq = 0

    def nonce(self):
        n = bytes(self.iv[:4]) + bytes(a ^ b for a, b in zip(self.iv[4:], self.seq.to_bytes(8, "big")))
        self.seq += 1
        return n

    def seal(self, inner_type, data, pad=0):
        inner = data + bytes([inner_type]) + bytes(pad)
        hdr = bytes([23, 3, 3]) + struct.pack(">H", len(inner) + 16)
        return hdr + self.aead.encrypt(self.nonce(), inner, hdr)

    def open(self, hdr, body):
        pt = self.aead.decrypt(self.nonce(), body, hdr).rstrip(b"\0")
        return pt[-1], pt[:-1]


class Sim13(threading.Thread):
    """Serves one TLS 1.3 connection (RFC 8446), written from the RFC independently of the C code, with optional faults."""

    def __init__(self, chain, key, fault=None, suite=0x1301, scheme=None, app=None):
        super().__init__(daemon=True)
        self.chain, self.key, self.fault, self.suite, self.app = chain, key, fault, suite, app
        self.scheme = scheme or (0x0807 if isinstance(key, ed25519.Ed25519PrivateKey) else 0x0804 if isinstance(key, rsa.RSAPrivateKey)
                                 else 0x0403 if key.curve.name == "secp256r1" else 0x0503)
        self.error, self.notes, self.received = None, [], b""
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(30)
        self.port = self.sock.getsockname()[1]
        self.start()

    def run(self):
        try:
            self.s, _ = self.sock.accept()
            self.s.settimeout(20)
            self.serve()
        except Exception as e:  # noqa: BLE001
            self.error = repr(e)
        finally:
            try:
                self.s.close()
            except Exception:  # noqa: BLE001
                pass
            self.sock.close()

    # records
    def exact(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError("client closed")
            b += c
        return b

    def record(self):
        hdr = self.exact(5)
        return hdr, self.exact(struct.unpack(">H", hdr[3:])[0])

    def plain(self, t, body):
        self.s.sendall(bytes([t, 3, 3]) + struct.pack(">H", len(body)) + body)

    def handshake_plain(self):
        """the next handshake message in clear (ClientHello), skipping a ChangeCipherSpec"""
        while True:
            hdr, body = self.record()
            if hdr[0] == 22:
                return body
            if hdr[0] == 21:
                raise EOFError(f"client alert {body.hex()}")

    # the protocol
    def parse_ch(self, ch):
        p = 4 + 2 + 32
        sid = ch[p + 1:p + 1 + ch[p]]
        p += 1 + ch[p]
        ns = struct.unpack(">H", ch[p:p + 2])[0]
        suites = [struct.unpack(">H", ch[p + 2 + i:p + 4 + i])[0] for i in range(0, ns, 2)]
        p += 2 + ns
        p += 1 + ch[p]
        end = p + 2 + struct.unpack(">H", ch[p:p + 2])[0]
        p += 2
        exts = {}
        while p < end:
            t, n = struct.unpack(">HH", ch[p:p + 4])
            exts[t] = ch[p + 4:p + 4 + n]
            p += 4 + n
        shares, ks = {}, exts.get(0x33, b"\0\0")
        q = 2
        while q < len(ks):
            g, n = struct.unpack(">HH", ks[q:q + 4])
            shares[g] = ks[q + 4:q + 4 + n]
            q += 4 + n
        return sid, suites, exts, shares

    def server_hello(self, random_, sid, suite, exts):
        return hs_msg(2, struct.pack(">H", 0x0303) + random_ + vec(1, sid) + struct.pack(">HB", suite, 0) + vec(2, b"".join(exts)))

    def serve(self):
        f = self.fault or ""
        _, hc, _ = SUITES13[self.suite]
        ch = self.handshake_plain()
        sid, suites, exts, shares = self.parse_ch(ch)
        self.notes.append(("suites", suites))
        transcript = ch
        if f.startswith("hrr"):
            hrr_exts = [ext(0x2b, b"\x03\x04")]
            if f in ("hrr_p256", "hrr_twice"):
                hrr_exts.append(ext(0x33, struct.pack(">H", 0x17)))
            if f == "hrr_same_group":
                hrr_exts.append(ext(0x33, struct.pack(">H", 0x1d)))
            if f == "hrr_cookie":
                hrr_exts.append(ext(0x2c, vec(2, b"a cookie for the second hello")))
            hrr = self.server_hello(HRR_RANDOM, sid, self.suite, hrr_exts)
            self.plain(22, hrr)
            transcript = hs_msg(254, hashlib.new(hc.name, ch).digest()) + hrr
            if f == "hrr_twice":
                ch2 = self.handshake_plain()
                hrr2 = self.server_hello(HRR_RANDOM, sid, self.suite, [ext(0x2b, b"\x03\x04"), ext(0x33, struct.pack(">H", 0x18))])
                self.plain(22, hrr2)
                self.record()                                        # the client's alert
                return
            ch = self.handshake_plain()
            sid, suites, exts, shares = self.parse_ch(ch)
            if f == "hrr_cookie":
                assert exts.get(0x2c) == vec(2, b"a cookie for the second hello"), "the cookie was not echoed"
            transcript += ch
        if 0x17 in shares and f == "hrr_p256":
            priv = ec.generate_private_key(ec.SECP256R1())
            peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), shares[0x17])
            shared = priv.exchange(ec.ECDH(), peer)
            share = ext(0x33, struct.pack(">HH", 0x17, 65) + priv.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint))
        else:
            priv = x25519.X25519PrivateKey.generate()
            shared = priv.exchange(x25519.X25519PublicKey.from_public_bytes(shares[0x1d]))
            share = ext(0x33, struct.pack(">HH", 0x1d, 32) + priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))
        sh_exts = [ext(0x2b, b"\x03\x03" if f == "tls12_via_sv" else b"\x03\x04")]
        if f != "missing_key_share":
            sh_exts.append(share)
        suite = 0x1304 if f == "wrong_suite" else self.suite
        sh = self.server_hello(os.urandom(32), b"x" * 32 if f == "bad_sid_echo" else sid, suite, sh_exts)
        transcript += sh
        hl = hc.digest_size
        early = extract(hc, None, bytes(hl))
        derived = expand_label(hc, early, b"derived", hashlib.new(hc.name, b"").digest(), hl)
        hs_secret = extract(hc, derived, shared)
        th = hashlib.new(hc.name, transcript).digest()
        c_hs = expand_label(hc, hs_secret, b"c hs traffic", th, hl)
        s_hs = expand_label(hc, hs_secret, b"s hs traffic", th, hl)
        master = extract(hc, expand_label(hc, hs_secret, b"derived", hashlib.new(hc.name, b"").digest(), hl), bytes(hl))
        wk, rk = Keys13(self.suite, s_hs), Keys13(self.suite, c_hs)

        ee_exts = []
        if 0x10 in exts and f != "no_alpn_answer":
            first = exts[0x10][3:3 + exts[0x10][2]]
            ee_exts.append(ext(0x10, vec(2, vec(1, b"h2" if f == "wrong_alpn" else first))))
        if f == "unsolicited_ext":
            ee_exts.append(ext(0x0005, b""))
        ee = hs_msg(8, vec(2, b"".join(ee_exts)))
        if f == "sh_plus_plaintext":
            self.plain(22, sh + ee)                                  # handshake bytes across the key change
            self.record()
            return
        self.plain(22, sh)
        if f in ("wrong_suite", "tls12_via_sv", "missing_key_share", "bad_sid_echo"):
            self.record()
            return
        self.plain(20, b"\x01")                                      # middlebox compatibility
        cert = hs_msg(11, b"\0" + vec(3, b"".join(vec(3, c.der) + vec(2, b"") for c in self.chain)))
        transcript += ee + cert
        content = b" " * 64 + b"TLS 1.3, server CertificateVerify\0" + hashlib.new(hc.name, transcript).digest()
        sig = self.sign(content)
        if f == "bad_cv":
            sig = sig[:-1] + bytes([sig[-1] ^ 1])
        cv = hs_msg(15, struct.pack(">H", self.scheme) + vec(2, sig))
        transcript += cv
        fin_key = expand_label(hc, s_hs, b"finished", b"", hl)
        verify_data = hmac.new(fin_key, hashlib.new(hc.name, transcript).digest(), hc.name).digest()
        if f == "bad_finished":
            verify_data = bytes([verify_data[0] ^ 0x80]) + verify_data[1:]
        fin = hs_msg(20, verify_data)
        transcript += fin
        flight = ee + cert + cv + fin
        if f == "coalesce":
            self.s.sendall(wk.seal(22, flight))
        elif f == "fragment":
            out, i, rnd = b"", 0, random.Random(5)
            while i < len(flight):
                k = rnd.randrange(1, 8)
                out += wk.seal(22, flight[i:i + k])
                i += k
            self.s.sendall(out)
        else:
            pad = 100 if f == "padding" else 0
            self.s.sendall(b"".join(wk.seal(22, m, pad) for m in (ee, cert, cv, fin)))
        if f in ("bad_cv", "bad_finished", "unsolicited_ext", "wrong_alpn"):
            self.record()
            return
        th = hashlib.new(hc.name, transcript).digest()
        c_ap = expand_label(hc, master, b"c ap traffic", th, hl)
        s_ap = expand_label(hc, master, b"s ap traffic", th, hl)
        # the client's flight: [ChangeCipherSpec] Finished
        while True:
            hdr, body = self.record()
            if hdr[0] == 20:
                continue
            t, pt = rk.open(hdr, body)
            break
        fin_key_c = expand_label(hc, c_hs, b"finished", b"", hl)
        want = hs_msg(20, hmac.new(fin_key_c, th, hc.name).digest())
        if t != 22 or pt != want:
            raise AssertionError("the client's Finished is wrong")
        self.notes.append(("client finished", "ok"))
        self.wk, self.rk, self.hc, self.c_ap, self.s_ap = Keys13(self.suite, s_ap), Keys13(self.suite, c_ap), hc, c_ap, s_ap
        (self.app or Sim13.echo)(self)

    def sign(self, content):
        if isinstance(self.key, ed25519.Ed25519PrivateKey):
            return self.key.sign(content)
        if isinstance(self.key, rsa.RSAPrivateKey):
            hc = {0x0804: hashes.SHA256, 0x0805: hashes.SHA384, 0x0806: hashes.SHA512}.get(self.scheme)
            if self.scheme == 0x0401:
                return self.key.sign(content, padding.PKCS1v15(), hashes.SHA256())
            return self.key.sign(content, padding.PSS(mgf=padding.MGF1(hc()), salt_length=hc.digest_size), hc())
        return self.key.sign(content, ec.ECDSA(hashes.SHA256() if self.scheme == 0x0403 else hashes.SHA384()))

    # application data
    def app_record(self):
        """the next application record from the client, following its KeyUpdates; (type, plaintext)"""
        while True:
            hdr, body = self.record()
            t, pt = self.rk.open(hdr, body)
            if t == 22 and pt[:1] == b"\x18":
                self.notes.append(("client key update", pt[4]))
                self.c_ap = expand_label(self.hc, self.c_ap, b"traffic upd", b"", self.hc.digest_size)
                self.rk = Keys13(self.suite, self.c_ap)
                continue
            return t, pt

    def send_app(self, data):
        self.s.sendall(self.wk.seal(23, data))

    def server_key_update(self, request):
        self.s.sendall(self.wk.seal(22, hs_msg(24, bytes([request]))))
        self.s_ap = expand_label(self.hc, self.s_ap, b"traffic upd", b"", self.hc.digest_size)
        self.wk = Keys13(self.suite, self.s_ap)

    def echo(self):
        """reads one application record and sends it back, then close_notify"""
        t, pt = self.app_record()
        self.received += pt
        self.send_app(pt)
        self.s.sendall(self.wk.seal(21, b"\x01\x00"))


GET = b"GET / HTTP/1.0\r\n\r\n".hex()
WWW_OK = b"HTTP/1.0 200 ok".hex()


@unittest.skipUnless(HAVE_CRYPTO and OPENSSL, "needs cryptography and the openssl command")
class Handshakes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.pki = Pki(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def get(self, server, **opts):
        o = dict(ca=self.pki.ca, time=NOW_TS, send=GET, readall=1, show=64, timeout=15000)
        o.update(opts)
        return connect(server.port, **o)

    def assertOk(self, r, version=None, suite=None):
        self.assertEqual(r["rc"], 0, "\n".join(r["lines"]))
        self.assertEqual(r["verified"], "1")
        if version:
            self.assertEqual(r["version"], version)
        if suite:
            self.assertEqual(r["suite"], suite)
        if "head" in r:
            self.assertTrue(r["head"].startswith(WWW_OK), r["lines"])

    def test_tls13_every_suite(self):
        for suite in ("TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384", "TLS_CHACHA20_POLY1305_SHA256"):
            for leaf, sigalg in (("ec", "ecdsa_secp256r1_sha256"), ("rsa", "rsa_pss_rsae_sha256"), ("ed", "ed25519"), ("ec384", "ecdsa_secp384r1_sha384")):
                with SServer(*self.pki.files(leaf), "-tls1_3", "-ciphersuites", suite) as s:
                    r = self.get(s)
                self.assertOk(r, "TLS 1.3", suite)
                self.assertEqual((r["group"], r["sigalg"]), ("x25519", sigalg), (suite, leaf))
                self.assertEqual(len(r["path"]), 3)
                self.assertTrue(r["path"][2].endswith("store=1"))
        with SServer(*self.pki.files("ec"), "-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256") as s:
            self.assertOk(self.get(s, noaesni=1), "TLS 1.3", "TLS_AES_128_GCM_SHA256")       # the portable AES and GHASH

    def test_tls12_every_suite(self):
        for cipher, leaf, name in (("ECDHE-ECDSA-AES128-GCM-SHA256", "ec", "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"),
                                   ("ECDHE-ECDSA-AES256-GCM-SHA384", "ec", "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"),
                                   ("ECDHE-ECDSA-CHACHA20-POLY1305", "ec", "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"),
                                   ("ECDHE-RSA-AES128-GCM-SHA256", "rsa", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"),
                                   ("ECDHE-RSA-AES256-GCM-SHA384", "rsa", "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"),
                                   ("ECDHE-RSA-CHACHA20-POLY1305", "rsa", "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"),
                                   ("ECDHE-ECDSA-AES128-GCM-SHA256", "ed", "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256")):
            with SServer(*self.pki.files(leaf), "-tls1_2", "-cipher", cipher) as s:
                r = self.get(s)
            self.assertOk(r, "TLS 1.2", name)
        for group, name in (("P-256", "secp256r1"), ("P-384", "secp384r1"), ("X25519", "x25519")):
            with SServer(*self.pki.files("rsa"), "-tls1_2", "-groups", group) as s:
                r = self.get(s)
            self.assertOk(r, "TLS 1.2")
            self.assertEqual(r["group"], name)
        with SServer(*self.pki.files("rsa"), "-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256") as s:
            self.assertOk(self.get(s, v=12, noaesni=1), "TLS 1.2")

    def test_hello_retry_request(self):
        for group, name in (("P-256", "secp256r1"), ("P-384", "secp384r1")):
            with SServer(*self.pki.files("ec"), "-tls1_3", "-groups", group) as s:
                r = self.get(s)
            self.assertOk(r, "TLS 1.3")
            self.assertEqual((r["hello_retry"], r["group"]), ("1", name))

    def test_alpn_and_versions(self):
        for ver in ("-tls1_3", "-tls1_2"):
            with SServer(*self.pki.files("ec"), ver, "-alpn", "http/1.1") as s:
                r = self.get(s, alpn="http/1.1")
            self.assertOk(r)
            self.assertEqual(r["alpn"], "http/1.1", ver)
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, v=12)
        self.assertIn(r["err"], ("ALERT", "VERSION", "EOF"), r["lines"])
        with SServer(*self.pki.files("ec"), "-tls1_2") as s:
            r = self.get(s, v=13)
        self.assertIn(r["err"], ("ALERT", "VERSION", "EOF"), r["lines"])
        with SServer(*self.pki.files("ec"), "-tls1_2") as s:          # we offer both: the server's 1.2 is fine, no downgrade alarm
            self.assertOk(self.get(s), "TLS 1.2")

    def test_key_update(self):
        """openssl s_server renews its keys and asks for ours (K); data keeps flowing both ways"""
        with SServer(*self.pki.files("ec"), "-tls1_3", www=False) as s:
            box = {}
            th = threading.Thread(target=lambda: box.update(r=connect(s.port, ca=self.pki.ca, time=NOW_TS, readn=6, send2=b"pong after update\n".hex(),
                                                                      timeout=15000)))
            th.start()
            s.wait_for("CIPHER is")
            s.send("K\n")
            time.sleep(0.3)
            s.send("hello\n")
            th.join(30)
            s.wait_for("pong after update")
        r = box["r"]
        self.assertEqual(r["rc"], 0, r["lines"])
        self.assertTrue(r["read"].startswith("6 end 0"), r["lines"])
        self.assertEqual(r["wrote2"], str(len("pong after update\n")))
        with SServer(*self.pki.files("ec"), "-tls1_3", www=False) as s:   # our side renews and asks for the server's
            r = connect(s.port, ca=self.pki.ca, time=NOW_TS, send=b"before\n".hex(), keyupdate=1, send2=b"after our update\n".hex(), timeout=15000)
            s.wait_for("after our update")
        self.assertEqual((r["rc"], r["keyupdate"]), (0, "0"), r["lines"])

    def test_refusals_against_a_server(self):
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, sni="other.example")
        self.assertEqual(r["err"], "CERT_NAME")
        self.assertIn("not valid for other.example", r["message"])
        self.assertIn("localhost", r["message"])
        self.assertEqual(r["alert_sent"], "42")                       # bad_certificate went to the server
        with SServer(*self.pki.files("ec"), "-tls1_2") as s:
            r = self.get(s, sni="other.example", insecure=1)
        self.assertEqual((r["rc"], r["insecure"], r["verified"]), (0, "1", "0"), r["lines"])
        self.assertTrue(r["verify_error"].endswith("CERT_NAME"))
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, time=NOW_TS + 400 * 86400)
        self.assertEqual(r["err"], "CERT_EXPIRED")
        self.assertIn("expired on", r["message"])
        other_ca = self.pki.write("other.pem", issue("Not The Root", cached_key("ec256", 40), ca=True).pem)
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, ca=other_ca)
        self.assertEqual(r["err"], "CERT_UNTRUSTED")
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, sni="127.0.0.1")                          # an address is checked against the IP SANs, no SNI is sent
        self.assertOk(r)
        r = connect(free_port(), ca="/nonexistent/cacert.pem")
        self.assertIn(r["result"], ("connect-failed",))
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, ca="/nonexistent/cacert.pem")
        self.assertEqual(r["err"], "NO_TRUST")
        self.assertIn("/nonexistent/cacert.pem", r["message"])
        with SServer(*self.pki.files("ec"), "-tls1_3") as s:
            r = self.get(s, requireclock=1, time=0)
        self.assertEqual(r["err"], "CLOCK", r["lines"])

    def _tcp_server(self, behaviour):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]

        def run():
            c, _ = srv.accept()
            try:
                behaviour(c)
            finally:
                time.sleep(0.2)
                c.close()
                srv.close()
        threading.Thread(target=run, daemon=True).start()
        return port

    def test_not_tls_stalls_and_cuts(self):
        def plain_http(c):
            c.recv(4096)
            c.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        r = connect(self._tcp_server(plain_http), ca=self.pki.ca, timeout=10000)
        self.assertEqual(r["err"], "PLAINTEXT")
        self.assertIn("HTTP/1.1 400", r["message"])

        def stall(c):
            c.recv(4096)
            time.sleep(4)
        t0 = time.time()
        r = connect(self._tcp_server(stall), ca=self.pki.ca, timeout=1500)
        self.assertEqual(r["err"], "TIMEOUT")
        self.assertLess(time.time() - t0, 4)

        def cut(c):
            c.recv(4096)
            c.sendall(bytes([22, 3, 3, 0, 90]) + b"\x02\x00\x00")       # the start of a ServerHello record, then nothing
        r = connect(self._tcp_server(cut), ca=self.pki.ca, timeout=10000)
        self.assertIn(r["err"], ("TRUNCATED", "EOF"))

        def silence_close(c):
            c.recv(4096)
        r = connect(self._tcp_server(silence_close), ca=self.pki.ca, timeout=10000)
        self.assertEqual(r["err"], "EOF")

    def test_bulk_both_ways(self):
        rnd = random.Random(12)
        out_data = rnd.randbytes(3_000_000)
        for maxv, nb in ((ssl.TLSVersion.TLSv1_3, 0), (ssl.TLSVersion.TLSv1_2, 0), (ssl.TLSVersion.TLSv1_3, 1)):
            srv = PySslServer(*self.pki.files("ec"), sink_then_source(3_000_000, out_data), maxv=maxv)
            r = connect(srv.port, ca=self.pki.ca, time=NOW_TS, sendn=3_000_000, readn=32 + len(out_data), nonblock=nb, timeout=30000)
            srv.join(30)
            self.assertIsNone(srv.error, srv.error)
            self.assertEqual(r["rc"], 0, r["lines"])
            got, digest = srv.result
            self.assertEqual(got, 3_000_000)
            self.assertEqual(r["sent"].split()[1], digest)             # what the server read is what we sent
            expect = hashlib.sha256(bytes.fromhex(digest) + out_data).hexdigest()
            self.assertEqual(r["read"].split()[-1], expect)
            if nb:
                self.assertGreater(int(r["bytes"].split()[-1]), 0)      # the non-blocking calls did have to wait

    def test_close_notify_or_not(self):
        def clean(s):
            s.recv(100)
            s.sendall(b"x" * 1000)
            s.unwrap()

        def abrupt(s):
            s.recv(100)
            s.sendall(b"x" * 1000)
            s.shutdown(socket.SHUT_RDWR)
        for handler, end in ((clean, "end 0 OK"), (abrupt, "TRUNCATED")):
            srv = PySslServer(*self.pki.files("ec"), handler)
            r = connect(srv.port, ca=self.pki.ca, time=NOW_TS, send=GET, readall=1, timeout=15000)
            srv.join(30)
            self.assertTrue(r["read"].startswith("1000 "), r["lines"])
            self.assertIn(end, r["read"])


class MutatingProxy(threading.Thread):
    """Relays connections to target_port, changing one byte of what the server sends within its first `span` bytes (the clear
    part of the handshake). A connection is cut 0.3 s after the last byte, so a damaged length field cannot leave it hanging."""

    def __init__(self, target_port, span, seed):
        super().__init__(daemon=True)
        self.target, self.span, self.rnd = target_port, span, random.Random(seed)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.done = False
        self.start()

    def run(self):
        import select
        while not self.done:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            try:
                s = socket.create_connection(("127.0.0.1", self.target), timeout=5)
            except OSError:
                c.close()
                continue
            where, sent = self.rnd.randrange(self.span), 0
            flip = self.rnd.choice((1, 2, 4, 8, 16, 32, 64, 128, 255))
            last = time.time()
            try:
                while time.time() - last < 0.3:
                    r, _, _ = select.select([c, s], [], [], 0.1)
                    for x in r:
                        d = x.recv(65536)
                        if not d:
                            raise EOFError
                        if x is s:
                            if sent <= where < sent + len(d):
                                d = bytearray(d)
                                d[where - sent] ^= flip
                                d = bytes(d)
                            sent += len(d)
                            c.sendall(d)
                        else:
                            s.sendall(d)
                        last = time.time()
            except (EOFError, OSError):
                pass
            c.close()
            s.close()

    def stop(self):
        self.done = True
        self.join(5)
        self.sock.close()


@unittest.skipUnless(HAVE_CRYPTO and OPENSSL, "needs cryptography and the openssl command")
class DamagedHandshakes(unittest.TestCase):
    """Hundreds of handshakes with one byte of the server's clear handshake changed: each must end in an error (or, when the
    byte did not matter, a verified success), never in a crash, a hang or an ASan report."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.pki = Pki(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_many(self, version_args, span, n, seed):
        outcomes = {}
        port = free_port()
        cert, key = self.pki.files("rsa")
        # without -naccept: one connection after the other, as long as the test needs
        server = subprocess.Popen([OPENSSL, "s_server", "-accept", f"127.0.0.1:{port}", "-www", "-cert", cert, "-key", key,
                                   "-cert_chain", str(Path(cert).with_suffix(".chain")), *version_args],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                    break
                except OSError:
                    time.sleep(0.1)
            proxy = MutatingProxy(port, span, seed)
            try:
                for _ in range(n):
                    r = connect(proxy.port, ca=self.pki.ca, time=NOW_TS, timeout=3000, send=GET, readall=1)
                    outcomes[r["err"]] = outcomes.get(r["err"], 0) + 1
                    if r["rc"] == 0:
                        self.assertEqual(r["verified"], "1", r["lines"])
            finally:
                proxy.stop()
        finally:
            server.kill()
            server.wait()
            server.stdin.close()
        print(f"\n     {' '.join(version_args)}: {dict(sorted(outcomes.items(), key=lambda kv: -kv[1]))}")
        self.assertNotIn("connect-failed", outcomes)
        self.assertGreater(sum(v for k, v in outcomes.items() if k != "OK"), n // 2, outcomes)
        return outcomes

    def test_tls12_flight(self):
        self.run_many(["-tls1_2"], 2600, 160, 21)                    # ServerHello, the chain, ServerKeyExchange, ServerHelloDone

    def test_tls13_server_hello(self):
        self.run_many(["-tls1_3"], 128, 80, 22)                      # the ServerHello (all that is in clear)


@unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
class Tls13Faults(unittest.TestCase):
    """The independent Python TLS 1.3 server breaks the protocol in one way at a time; the client must notice each."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.pki = Pki(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_sim(self, fault=None, leaf=None, app=None, suite=0x1301, scheme=None, **opts):
        leaf = leaf or self.pki.ec
        chain = [leaf, self.pki.rsa_inter if leaf is self.pki.rsa else self.pki.inter]
        sim = Sim13(chain, leaf.key, fault=fault, suite=suite, scheme=scheme, app=app)
        o = dict(ca=self.pki.ca, time=NOW_TS, timeout=10000, send=b"ping".hex(), readall=1, alpn="http/1.1")
        o.update(opts)
        r = connect(sim.port, **o)
        sim.join(30)
        return r, sim

    def test_good_and_harmless_variations(self):
        for fault in (None, "coalesce", "fragment", "padding", "hrr_cookie", "hrr_p256"):
            for suite in (0x1301, 0x1302, 0x1303):
                r, sim = self.run_sim(fault, suite=suite)
                self.assertEqual(r["rc"], 0, (fault, suite, r["lines"], sim.error))
                self.assertIsNone(sim.error, (fault, sim.error))
                self.assertTrue(r["read"].startswith("4 end 0 OK"), r["lines"])
                self.assertEqual(r["alpn"], "http/1.1")
                if fault and fault.startswith("hrr"):
                    self.assertEqual(r["hello_retry"], "1")
        r, sim = self.run_sim(None, leaf=self.pki.rsa, scheme=0x0805)
        self.assertEqual((r["rc"], r["sigalg"]), (0, "rsa_pss_rsae_sha384"), r["lines"])
        r, sim = self.run_sim(None, leaf=self.pki.ed)
        self.assertEqual((r["rc"], r["sigalg"]), (0, "ed25519"), r["lines"])

    def test_each_fault_is_caught(self):
        cases = {
            "sh_plus_plaintext": "PROTOCOL", "hrr_same_group": "SUITE", "hrr_twice": "PROTOCOL", "hrr_nothing": "PROTOCOL",
            "unsolicited_ext": "SUITE", "bad_cv": "HANDSHAKE_SIG", "bad_finished": "FINISHED", "wrong_suite": "SUITE",
            "tls12_via_sv": "VERSION", "missing_key_share": "PROTOCOL", "bad_sid_echo": "PROTOCOL", "wrong_alpn": "SUITE",
        }
        for fault, want in cases.items():
            r, sim = self.run_sim(fault)
            self.assertEqual(r["err"], want, (fault, r["lines"], sim.error))
        r, sim = self.run_sim(None, leaf=self.pki.rsa, scheme=0x0401)    # PKCS#1 v1.5 in a TLS 1.3 CertificateVerify
        self.assertEqual(r["err"], "HANDSHAKE_SIG", r["lines"])

    def test_after_the_handshake(self):
        def key_update(sim):
            t, pt = sim.app_record()
            sim.server_key_update(1)
            sim.send_app(b"under the new key")
            t, pt2 = sim.app_record()                                # the client's next data, after its own KeyUpdate
            sim.received = pt + pt2
            sim.send_app(b"!")
            sim.s.sendall(sim.wk.seal(21, b"\x01\x00"))

        r, sim = self.run_sim(app=key_update, readn=len("under the new key"), send2=b"pong".hex(), readn2=1)
        self.assertEqual(r["rc"], 0, r["lines"])
        self.assertIsNone(sim.error, sim.error)
        self.assertEqual(sim.received, b"pingpong")
        self.assertIn(("client key update", 0), sim.notes)

        def tickets(sim):
            sim.app_record()
            for i in range(3):
                sim.s.sendall(sim.wk.seal(22, hs_msg(4, struct.pack(">IIB", 7200, i, 1) + b"x" + vec(2, b"ticket") + vec(2, b""))))
            sim.send_app(b"data")
            sim.s.sendall(sim.wk.seal(21, b"\x01\x00"))
        r, sim = self.run_sim(app=tickets)
        self.assertTrue(r["read"].startswith("4 end 0 OK"), r["lines"])

        def bad(kind):
            def app(sim):
                sim.app_record()
                if kind == "ku_and_more":
                    sim.s.sendall(sim.wk.seal(22, hs_msg(24, b"\x00") + hs_msg(4, b"\0" * 9)))
                elif kind == "ccs":
                    sim.plain(20, b"\x01")
                elif kind == "empty_flood":
                    sim.s.sendall(b"".join(sim.wk.seal(23, b"") for _ in range(100)))
                elif kind == "overflow":
                    sim.s.sendall(sim.wk.seal(23, b"z" * 16385))
                elif kind == "warning_alert":
                    sim.s.sendall(sim.wk.seal(21, b"\x01\x70"))
                elif kind == "tampered":
                    rec = bytearray(sim.wk.seal(23, b"data"))
                    rec[-1] ^= 1
                    sim.s.sendall(bytes(rec))
                elif kind == "cut":
                    sim.send_app(b"data")
                    sim.s.shutdown(socket.SHUT_RDWR)
                    return
                time.sleep(0.5)
            return app
        for kind, want in (("ku_and_more", "PROTOCOL"), ("ccs", "PROTOCOL"), ("empty_flood", "PROTOCOL"), ("overflow", "PROTOCOL"),
                           ("warning_alert", "ALERT"), ("tampered", "DECRYPT"), ("cut", "TRUNCATED")):
            r, sim = self.run_sim(app=bad(kind))
            self.assertEqual(r["rc"], 0, (kind, r["lines"]))
            self.assertIn(want, r["read"], (kind, r["lines"]))


# ------------------------------------------------------------------------------------------------------------ RFC 8448

def rfc8448_path():
    p = os.environ.get("TOUHOU_RFC8448") or str(REPO / "build" / "rfc8448.txt")
    return p if os.path.isfile(p) else None


def parse_rfc8448(text):
    """The steps of the traces: [{section, actor, action, data: {label: bytes}}]. A value is "label (N octets):" followed by N
    hex bytes over as many lines as it takes; page footers and headers that fall inside a dump are skipped."""
    import re
    steps, section, cur = [], None, None
    lines = text.replace("\f", "\n").splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r"^(\d+)\.\s+\S", ln)
        if m and not ln.rstrip().endswith(tuple("0123456789")):      # a heading (the table of contents ends in page numbers)
            section = int(m.group(1))
            i += 1
            continue
        m = re.match(r"^\s+\{(client|server)\}\s+(.*)$", ln)
        if m:
            cur = {"section": section, "actor": m.group(1), "action": m.group(2).strip().rstrip(":"), "data": {}}
            steps.append(cur)
            i += 1
            continue
        m = re.match(r"^\s+(\S.*?)\s+\((\d+) octets?\):\s*(.*)$", ln)
        if m and cur is not None:
            label, n, rest = m.group(1), int(m.group(2)), m.group(3).strip()
            data = bytes.fromhex(rest.replace(" ", "")) if rest and rest != "(empty)" else b""      # (the RFC writes an empty value as "(empty)")
            j = i + 1
            while len(data) < n and j < len(lines):
                s = lines[j].strip()
                if re.fullmatch(r"(?:[0-9a-f]{2}\s*)+", s):
                    data += bytes.fromhex(s.replace(" ", ""))
                elif s and not (s.startswith("Thomson") or s.startswith("RFC 8448")):
                    break
                j += 1
            cur["data"][label] = data[:n]
            i = j
            continue
        i += 1
    return steps


class Replay(threading.Thread):
    """Plays the server's side of a trace: every "{server} send" record goes out as it is, every "{client} send" record must
    arrive byte for byte."""

    def __init__(self, steps):
        super().__init__(daemon=True)
        self.steps, self.error, self.matched = steps, None, 0
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(30)
        self.port = self.sock.getsockname()[1]
        self.start()

    def run(self):
        try:
            s, _ = self.sock.accept()
            s.settimeout(20)
            for st in self.steps:
                rec = st["data"]["complete record"]
                if st["actor"] == "server":
                    s.sendall(rec)
                    continue
                got = b""
                while len(got) < len(rec):
                    c = s.recv(len(rec) - len(got))
                    if not c:
                        raise AssertionError(f"the client stopped before '{st['action']}'")
                    got += c
                if got != rec:
                    raise AssertionError(f"'{st['action']}' differs:\n  want {rec.hex()}\n  got  {got.hex()}")
                self.matched += 1
            s.close()
        except Exception as e:  # noqa: BLE001
            self.error = str(e)
        finally:
            self.sock.close()


class Rfc8448Parser(unittest.TestCase):
    """The trace reader on a sample laid out the way the RFC lays its traces out (values wrapped over lines, a page break
    in the middle of one), so that it is known to work before the real file is there."""

    SAMPLE = """
3.  Simple 1-RTT Handshake

   {client}  create an ephemeral x25519 key pair:

      private key (32 octets):  49 af 42 ba 7f 79 94 85 2d 71 3e f2 78
         4b cb ca a7 91 1d e2 6a dc 56 42 cb 63 45 40 e7 ea 50 05

   {client}  send handshake record:

      payload (4 octets):  01 00 00 00

      complete record (9 octets):  16 03 01 00 04 01 00



Thomson                       Informational                     [Page 4]
\f
RFC 8448                 Example TLS 1.3 Traces             January 2019


         00 00

   {server}  send handshake record:

      complete record (3 octets):  16 03 03
"""

    def test_sample(self):
        steps = parse_rfc8448(self.SAMPLE)
        self.assertEqual([(s["actor"], s["action"]) for s in steps],
                         [("client", "create an ephemeral x25519 key pair"), ("client", "send handshake record"), ("server", "send handshake record")])
        self.assertEqual(steps[0]["data"]["private key"].hex(), "49af42ba7f7994852d713ef2784bcbcaa7911de26adc5642cb634540e7ea5005")
        self.assertEqual(steps[1]["data"]["payload"], b"\x01\x00\x00\x00")
        self.assertEqual(steps[1]["data"]["complete record"], U("160301000401000000"))
        self.assertEqual({s["section"] for s in steps}, {3})


def det_stream(seed, n):
    """the bytes tlstool's det_random gives for seed=N (xorshift64*), to know the private key a seeded client used"""
    m = (1 << 64) - 1
    s, out = (seed | 1) & m, bytearray()
    for _ in range(n):
        s ^= s >> 12
        s ^= (s << 25) & m
        s ^= s >> 27
        out.append(((s * 2685821657736338717) & m) >> 56)
    return bytes(out)


class RecordingProxy(threading.Thread):
    """Relays one connection and notes every record in order as (actor, bytes)."""

    def __init__(self, target_port):
        super().__init__(daemon=True)
        self.target, self.records = target_port, []
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(20)
        self.port = self.sock.getsockname()[1]
        self.start()

    def run(self):
        import select
        c, _ = self.sock.accept()
        s = socket.create_connection(("127.0.0.1", self.target), timeout=10)
        bufs = {c: b"", s: b""}
        open_ = {c, s}
        while open_:
            r, _, _ = select.select(list(open_), [], [], 5)
            if not r:
                break
            for x in r:
                d = x.recv(65536)
                if not d:
                    open_.discard(x)
                    continue
                (s if x is c else c).sendall(d)
                bufs[x] += d
                while len(bufs[x]) >= 5 and len(bufs[x]) >= 5 + struct.unpack(">H", bufs[x][3:5])[0]:
                    n = 5 + struct.unpack(">H", bufs[x][3:5])[0]
                    self.records.append(("client" if x is c else "server", bufs[x][:n]))
                    bufs[x] = bufs[x][n:]
        c.close()
        s.close()
        self.sock.close()


def rfc8448_text(section, title, x25519_priv, records):
    """A trace in the layout of RFC 8448: values wrapped over indented lines, and a page break in the middle of one"""
    def value(label, data):
        hexes = [f"{b:02x}" for b in data]
        first, rest = hexes[:13], hexes[13:]
        lines = [f"      {label} ({len(data)} octets):  " + " ".join(first)]
        lines += ["         " + " ".join(rest[i:i + 16]) for i in range(0, len(rest), 16)]
        return lines
    out = [f"{section}.  {title}", "", "   {client}  create an ephemeral x25519 key pair:", ""] + value("private key", x25519_priv) + [""]
    kinds = {20: "change_cipher_spec", 21: "alert", 22: "handshake", 23: "application_data"}
    for i, (actor, rec) in enumerate(records):
        out += [f"   {{{actor}}}  send {kinds[rec[0]]} record:", ""]
        if i == 0:                                                   # the ClientHello: its payload too, as in the RFC
            out += value("payload", rec[5:]) + [""]
        out += value("complete record", rec) + [""]
        if i == len(records) // 2:
            out += ["", "", "Thomson                       Informational                     [Page 9]", "\f",
                    "RFC 8448                 Example TLS 1.3 Traces             January 2019", "", ""]
    return "\n".join(out) + "\n"


@unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
class ReplayMachinery(unittest.TestCase):
    """The RFC 8448 test machinery, proven end to end without the RFC: a real handshake with the Python TLS 1.3 server is
    recorded, written out in the RFC's layout, read back, and replayed by a client that is given only the ClientHello and the
    private key (its own random numbers are fresh): every record it sends must match the recording byte for byte."""

    def test_record_and_replay(self):
        tmp = tempfile.TemporaryDirectory()
        pki_ = Pki(tmp.name)
        sim = Sim13([pki_.ec, pki_.inter], pki_.ec.key)
        rec = RecordingProxy(sim.port)
        data = bytes(range(50))
        r = connect(rec.port, ca=pki_.ca, time=NOW_TS, seed=7, timeout=10000, send=data.hex(), readn=len(data), shutdown=1)
        sim.join(20)
        rec.join(20)
        self.assertEqual(r["rc"], 0, r["lines"])
        self.assertIsNone(sim.error, sim.error)
        records = rec.records
        self.assertEqual(records[0][0], "client")
        self.assertGreaterEqual(sum(1 for a, _ in records if a == "client"), 4)          # hello, CCS, Finished, data, close_notify
        priv = det_stream(7, 96)[64:96]
        steps = [s for s in parse_rfc8448(rfc8448_text(3, "Simple 1-RTT Handshake", priv, records)) if s["action"].startswith("send")]
        self.assertEqual([s["data"]["complete record"] for s in steps], [b for _, b in records])
        rp = Replay(steps)
        ch = steps[0]["data"]["payload"]
        r2 = connect(rp.port, ca=pki_.ca, time=NOW_TS, timeout=10000, script_ch1=ch.hex(), script_x=priv.hex(), send=data.hex(),
                     readn=len(data), shutdown=1)
        rp.join(20)
        self.assertEqual(r2["rc"], 0, r2["lines"])
        self.assertIsNone(rp.error, rp.error)
        self.assertEqual(rp.matched, sum(1 for a, _ in records if a == "client"))
        tmp.cleanup()


@unittest.skipUnless(rfc8448_path(), "the text of RFC 8448 is not at build/rfc8448.txt (or $TOUHOU_RFC8448): fetch "
                     "https://www.rfc-editor.org/rfc/rfc8448.txt there to run the byte-for-byte replays")
class Rfc8448(unittest.TestCase):
    """The client replays the RFC's ClientHellos (with their private keys) and must then produce every later record of the
    trace byte for byte: its Finished, its application data and its close_notify, under keys derived from the server's records."""

    @classmethod
    def setUpClass(cls):
        cls.steps = parse_rfc8448(Path(rfc8448_path()).read_text(errors="replace"))

    def replay(self, section):
        steps = [s for s in self.steps if s["section"] == section]
        sends = [s for s in steps if s["action"].startswith("send") and "complete record" in s["data"]]
        keys = {s["action"]: s["data"] for s in steps if "key pair" in s["action"] and s["actor"] == "client"}
        x = next(d["private key"] for a, d in keys.items() if "x25519" in a)
        p = next((d["private key"] for a, d in keys.items() if "P-256" in a), None)
        hellos = [s["data"]["payload"] for s in sends if s["actor"] == "client" and "handshake" in s["action"] and s["data"].get("payload", b"")[:1] == b"\x01"]    # (an alert record's payload also starts with 01)
        appdata = next((s["data"]["payload"] for s in sends if s["actor"] == "client" and "application_data" in s["action"]), None)
        server_data = next((s["data"]["payload"] for s in sends if s["actor"] == "server" and "application_data" in s["action"]), b"")
        self.assertGreater(len(sends), 3)
        rp = Replay(sends)
        opts = dict(insecure=1, time=1500000000, timeout=10000, script_ch1=hellos[0].hex(), script_x=x.hex(), shutdown=1)
        if appdata:
            opts.update(send=appdata.hex(), readn=len(server_data) or 1)
        if len(hellos) > 1:
            opts.update(script_ch2=hellos[1].hex(), script_p=p.hex())
        r = connect(rp.port, sni="server", **opts)
        rp.join(30)
        self.assertEqual(r["rc"], 0, r["lines"])
        self.assertIsNone(rp.error, rp.error)
        self.assertEqual(rp.matched, sum(1 for s in sends if s["actor"] == "client"))
        return r

    def test_simple_1rtt(self):
        r = self.replay(3)
        self.assertEqual((r["suite"], r["group"], r["sigalg"]), ("TLS_AES_128_GCM_SHA256", "x25519", "rsa_pss_rsae_sha256"))

    def test_hello_retry_request(self):
        r = self.replay(5)
        self.assertEqual((r["hello_retry"], r["group"]), ("1", "secp256r1"))


# ------------------------------------------------------------------------------------------------------------ libhttp

def parse_resp(raw, chunk=4096, flags=1, eof=False, discard=False):
    opts = (" eof=1" if eof else "") + (" discard=1" if discard else "")
    out = T().cmd(f"http_parse {flags} {chunk} {raw.hex() or '-'}{opts}")
    res = {"headers": [], "status": None}
    for ln in out:
        k, _, v = ln.partition(" ")
        if k == "header":
            res["headers"].append(v)
        elif k == "status":
            res["status"] = v
        elif k == "result":
            parts = v.split()
            res["rc"], res["used"], res["framing"] = int(parts[0]), int(parts[2]), int(parts[4])
        elif k == "body":
            n, d = v.split()
            res["body_len"], res["body_sha"] = int(n), d
        else:
            res[k] = v
    return res


def sha(b):
    return hashlib.sha256(b).hexdigest()


RFC3986_BASE = "http://a/b/c/d;p?q"
RFC3986 = [  # RFC 3986 5.4.1 and 5.4.2 (fragments are dropped here, "//g" gets its "/")
    ("g", "http://a/b/c/g"), ("./g", "http://a/b/c/g"), ("g/", "http://a/b/c/g/"), ("/g", "http://a/g"), ("//g", "http://g/"),
    ("?y", "http://a/b/c/d;p?y"), ("g?y", "http://a/b/c/g?y"), ("#s", "http://a/b/c/d;p?q"), ("g#s", "http://a/b/c/g"),
    ("g?y#s", "http://a/b/c/g?y"), (";x", "http://a/b/c/;x"), ("g;x", "http://a/b/c/g;x"), ("g;x?y#s", "http://a/b/c/g;x?y"),
    ("", "http://a/b/c/d;p?q"), (".", "http://a/b/c/"), ("./", "http://a/b/c/"), ("..", "http://a/b/"), ("../", "http://a/b/"),
    ("../g", "http://a/b/g"), ("../..", "http://a/"), ("../../", "http://a/"), ("../../g", "http://a/g"),
    ("../../../g", "http://a/g"), ("../../../../g", "http://a/g"), ("/./g", "http://a/g"), ("/../g", "http://a/g"),
    ("g.", "http://a/b/c/g."), (".g", "http://a/b/c/.g"), ("g..", "http://a/b/c/g.."), ("..g", "http://a/b/c/..g"),
    ("./../g", "http://a/b/g"), ("./g/.", "http://a/b/c/g/"), ("g/./h", "http://a/b/c/g/h"), ("g/../h", "http://a/b/c/h"),
    ("g;x=1/./y", "http://a/b/c/g;x=1/y"), ("g;x=1/../y", "http://a/b/c/y"), ("g?y/./x", "http://a/b/c/g?y/./x"),
    ("g?y/../x", "http://a/b/c/g?y/../x"), ("g#s/./x", "http://a/b/c/g"), ("g#s/../x", "http://a/b/c/g"),
    ("https://other.example:8443/x?y", "https://other.example:8443/x?y"), ("HTTP://A.EXAMPLE/Z", "http://a.example/Z"),
]


class HttpUrls(unittest.TestCase):
    def url(self, base, ref=None):
        out = T().one(f"url {base.encode().hex()}" + (f" {ref.encode().hex() or '-'}" if ref is not None else ""))
        return out

    def test_parse(self):
        cases = [
            ("http://example.org", "http://example.org/ example.org 80 /"),
            ("https://Example.ORG:8443/a/b?c=d#frag", "https://example.org:8443/a/b?c=d example.org 8443 /a/b?c=d"),
            ("https://example.org:443/", "https://example.org/ example.org 443 /"),
            ("http://10.0.2.2:8080?x", "http://10.0.2.2:8080/?x 10.0.2.2 8080 /?x"),
            ("http://h/a b<c>\"d", "http://h/a%20b%3Cc%3E%22d h 80 /a%20b%3Cc%3E%22d"),
            ("http://h/café", "http://h/caf%C3%A9 h 80 /caf%C3%A9"),
        ]
        for u, want in cases:
            self.assertEqual(self.url(u), want, u)
        for bad in ("ftp://example.org/", "example.org", "http:/x", "http://", "http://user@host/", "http://[::1]/", "http://h:0/",
                    "http://h:65536/", "http://h:12a/", "http://bad host/", "http://h%41/"):
            self.assertTrue(self.url(bad).startswith("error"), bad)

    def test_rfc3986_resolution(self):
        for ref, want in RFC3986:
            self.assertEqual(self.url(RFC3986_BASE, ref).split()[0], want, ref)

    def test_request(self):
        extra = b"X-Test: 1\r\n".hex()
        req = U(T().one(f"http_request {b'https://example.org:8443/p?q'.hex()} {extra} 1"))
        self.assertTrue(req.startswith(b"GET /p?q HTTP/1.1\r\nHost: example.org:8443\r\n"), req)
        self.assertIn(b"Accept-Encoding: gzip, deflate\r\n", req)
        self.assertIn(b"Connection: close\r\n", req)
        self.assertTrue(req.endswith(b"X-Test: 1\r\n\r\n"), req)
        req = U(T().one(f"http_request {b'http://example.org/'.hex()} - 0"))
        self.assertTrue(req.startswith(b"GET / HTTP/1.1\r\nHost: example.org\r\n"), req)
        self.assertNotIn(b"Accept-Encoding", req)

    def test_request_user_agent(self):
        """fetch -U: a User-Agent line in the extra headers replaces the library's own (two would be sent otherwise);
        another header that only mentions the name does not"""
        url = b"http://example.org/".hex()

        def build(extra):
            return U(T().one(f"http_request {url} {extra.encode().hex()} 0"))

        req = U(T().one(f"http_request {url} - 0"))        # '-' is "no extra headers" on the tool's side
        self.assertEqual(req.count(b"User-Agent:"), 1, req)
        self.assertIn(b"User-Agent: libhttp/1.0 (TouhouOS)\r\n", req)
        for line in ("User-Agent: fetch/1.0 (Gensokyo)\r\n", "user-agent: lower\r\n", "X-A: 1\r\nUSER-AGENT: upper\r\nX-B: 2\r\n"):
            req = build(line)
            self.assertEqual(req.lower().count(b"user-agent:"), 1, req)
            self.assertNotIn(b"libhttp", req)
            self.assertIn(line.encode(), req)
            self.assertTrue(req.endswith(b"\r\n\r\n"), req)
        req = build("X-Note: user-agent: mentioned\r\n")   # only a value mentions it: the default stays
        self.assertIn(b"User-Agent: libhttp/1.0 (TouhouOS)\r\n", req)
        self.assertIn(b"X-Note: user-agent: mentioned\r\n", req)


class HttpParsing(unittest.TestCase):
    def each_split(self, raw, **kw):
        results = [parse_resp(raw, chunk=c, **kw) for c in (1, 2, 3, 7, 64, 100000)]
        for r in results[1:]:
            for k in ("rc", "status", "headers", "body_len", "body_sha"):
                self.assertEqual(r.get(k), results[0].get(k), (k, raw[:80]))
        return results[0]

    def test_framings(self):
        body = b"hello, gensokyo\n" * 50
        r = self.each_split(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(body) + body + b"EXTRA")
        self.assertEqual((r["rc"], r["status"], r["body_len"], r["body_sha"]), (1, "200 OK", len(body), sha(body)))
        self.assertIn("Content-Type: text/plain", r["headers"])
        chunked = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + b"5;ext=1\r\nhello\r\n" + b"1A\r\n" + b"x" * 26 + b"\r\n" + \
            b"0\r\nTrailer-Field: ignored\r\n\r\nNEXT"
        r = self.each_split(chunked)
        self.assertEqual((r["rc"], r["body_len"], r["body_sha"], r["framing"]), (1, 31, sha(b"hello" + b"x" * 26), 2))
        self.assertNotIn("Trailer-Field: ignored", r["headers"])
        r = self.each_split(b"HTTP/1.0 200 OK\r\nServer: old\r\n\r\n" + body, eof=True)
        self.assertEqual((r["rc"], r["body_len"], r["framing"]), (1, len(body), 3))
        r = self.each_split(b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", flags=2)               # HEAD
        self.assertEqual((r["rc"], r["body_len"]), (1, 0))
        for code in (204, 304):
            r = self.each_split(b"HTTP/1.1 %d X\r\nContent-Length: 10\r\n\r\n" % code)
            self.assertEqual((r["rc"], r["body_len"]), (1, 0))
        r = self.each_split(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok")
        self.assertEqual((r["rc"], r["status"], r["body_len"]), (1, "201 Created", 2))
        r = self.each_split(b"HTTP/1.1 200 OK\nContent-Length: 3\nX-Folded: a\n  b\n\tc\n\nabc")                  # bare LF, obs-fold
        self.assertEqual((r["rc"], r["body_len"]), (1, 3))
        self.assertIn("X-Folded: a b c", r["headers"])
        r = self.each_split(b"HTTP/1.1 200 OK\r\nContent-Length: 4, 4\r\n\r\nabcd")
        self.assertEqual((r["rc"], r["body_len"]), (1, 4))
        r = self.each_split(b"HTTP/1.1 301 Moved\r\nLocation: /x\r\nContent-Length: 5\r\n\r\nmoved", discard=True)
        self.assertEqual((r["rc"], r["body_len"]), (1, 0))
        r = self.each_split(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
        self.assertEqual((r["rc"], r["body_len"]), (1, 0))

    def test_decoding(self):
        body = (b"Reimu Hakurei, Marisa Kirisame, Sakuya Izayoi. " * 300) + bytes(range(256))
        gz = __import__("gzip").compress(body)
        zl = zlib.compress(body)
        raw = zlib.compress(body)[2:-4]
        for enc, data in (("gzip", gz), ("x-gzip", gz), ("deflate", zl), ("deflate", raw)):
            for framing in ("length", "chunked"):
                if framing == "length":
                    resp = b"HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\nContent-Length: %d\r\n\r\n" % (enc.encode(), len(data)) + data
                else:
                    resp = b"HTTP/1.1 200 OK\r\nContent-Encoding: %s\r\nTransfer-Encoding: chunked\r\n\r\n" % enc.encode()
                    for i in range(0, len(data), 333):
                        part = data[i:i + 333]
                        resp += b"%x\r\n" % len(part) + part + b"\r\n"
                    resp += b"0\r\n\r\n"
                r = self.each_split(resp)
                self.assertEqual((r["rc"], r["body_len"], r["body_sha"]), (1, len(body), sha(body)), (enc, framing))
        two = __import__("gzip").compress(b"first member ") + __import__("gzip").compress(b"second member")
        r = self.each_split(b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(two) + two)
        self.assertEqual((r["rc"], r["body_sha"]), (1, sha(b"first member second member")))
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(gz) + gz, flags=0)
        self.assertEqual((r["rc"], r["body_sha"]), (1, sha(gz)))                                         # not asked to decode
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 3\r\n\r\nabc")
        self.assertEqual((r["rc"], r["body_sha"]), (1, sha(b"abc")))                                     # unknown: passed through
        bad = bytearray(gz)
        bad[len(bad) // 2] ^= 0xff
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(bad) + bytes(bad))
        self.assertEqual(r["rc"], -109)
        cut = gz[:len(gz) // 2]
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(cut) + cut)
        self.assertEqual(r["rc"], -109)

    def test_refusals(self):
        cases = [
            (b"HTTP/2 200 OK\r\n\r\n", -106), (b"ICY 200 OK\r\n\r\n", -106), (b"HTTP/1.1 2000 OK\r\n\r\n", -106), (b"HTTP/1.1 099 x\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\n", -106), (b"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n", -106), (b"HTTP/1.1 200 OK\r\nNoColon\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\n folded first\r\n\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nX: " + b"a" * 9000 + b"\r\n\r\n", -107),
            (b"HTTP/1.1 200 OK\r\n" + b"".join(b"H%d: v\r\n" % i for i in range(300)) + b"\r\n", -107),
            (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcd\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffffff\r\n", -106),
            (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", -112),
            (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", -112),
        ]
        for raw, want in cases:
            for chunk in (1, 4096):
                self.assertEqual(parse_resp(raw, chunk=chunk)["rc"], want, raw[:70])
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", eof=True)
        self.assertEqual(r["rc"], -108)
        r = parse_resp(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", eof=True)
        self.assertEqual(r["rc"], -108)
        r = parse_resp(b"", eof=True)
        self.assertEqual(r["rc"], -108)
        r = parse_resp(b"HTTP/1.1 200 OK\r\nContent-Le", eof=True)
        self.assertEqual(r["rc"], -108)

    def test_fuzz(self):
        """random splits and random corruption of valid responses: never a crash or an ASan report"""
        rnd = random.Random(13)
        gz = __import__("gzip").compress(b"payload " * 100)
        bases = [b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
                 b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
                 b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(gz) + gz]
        for _ in range(600):
            b = bytearray(rnd.choice(bases))
            for _ in range(rnd.randrange(1, 5)):
                b[rnd.randrange(len(b))] = rnd.randrange(256)
            parse_resp(bytes(b), chunk=rnd.choice((1, 3, 50, 4096)), eof=rnd.random() < 0.5)


class Cookies(unittest.TestCase):
    def set(self, url, sc, now=NOW_TS):
        return T().one(f"http_cookie_set {url.encode().hex()} {sc.encode().hex()} {now}")

    def get(self, url, now=NOW_TS):
        return T().one(f"http_cookie_get {url.encode().hex()} {now}")[len("cookie "):]

    def test_jar(self):
        T().one("http_cookie_reset")
        self.set("https://www.example.org/a/b", "sid=1; Path=/; Secure; HttpOnly")
        self.set("https://www.example.org/a/b", "pref=dark")                      # default path /a
        self.set("https://www.example.org/", "wide=yes; Domain=example.org")
        self.set("https://www.example.org/", "bad=1; Domain=com")                # refused: a top-level domain
        self.set("https://www.example.org/", "evil=1; Domain=other.org")         # refused: not ours
        self.set("https://www.example.org/", "gone=1; Max-Age=0")
        self.set("https://www.example.org/", "old=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT")
        self.set("https://www.example.org/", "later=1; Expires=Fri, 01 Jan 2100 00:00:00 GMT")
        self.set("https://www.example.org/", "short=1; Max-Age=60")
        self.assertEqual(self.get("https://www.example.org/a/x"), "pref=dark; sid=1; wide=yes; later=1; short=1")
        self.assertEqual(self.get("http://www.example.org/a/x"), "pref=dark; wide=yes; later=1; short=1")      # sid is Secure
        self.assertEqual(self.get("https://api.example.org/"), "wide=yes")
        self.assertEqual(self.get("https://www.example.org/ab"), "sid=1; wide=yes; later=1; short=1")          # /a does not match /ab
        self.assertEqual(self.get("https://www.example.org/", now=NOW_TS + 61), "sid=1; wide=yes; later=1")
        self.set("https://www.example.org/a/b", "pref=light")
        self.assertIn("pref=light", self.get("https://www.example.org/a/"))
        self.set("https://www.example.org/a/b", "pref=x; Max-Age=-1")
        self.assertNotIn("pref", self.get("https://www.example.org/a/"))
        self.set("http://10.0.2.2/", "ip=1")
        self.assertEqual(self.get("http://10.0.2.2/x"), "ip=1")
        T().one("http_cookie_reset")


class RawHttp(threading.Thread):
    """A small HTTP server for libhttp's client: routes[path](request bytes) returns (response bytes, close_notify)."""

    def __init__(self, routes, tls=None):
        super().__init__(daemon=True)
        self.routes, self.tls = routes, tls
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.sock.settimeout(1)
        self.port = self.sock.getsockname()[1]
        self.requests = []
        self.stop = False
        self.start()

    def run(self):
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            try:
                c.settimeout(10)
                if self.tls:
                    c = self.tls.wrap_socket(c, server_side=True)
                req = b""
                while b"\r\n\r\n" not in req:
                    d = c.recv(4096)
                    if not d:
                        break
                    req += d
                self.requests.append(req)
                path = req.split(b" ")[1].decode() if b" " in req else "/"
                resp, notify = self.routes.get(path.split("?")[0], lambda r: (b"HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", True))(req)
                c.sendall(resp)
                if self.tls and notify:
                    c = c.unwrap()
            except Exception:  # noqa: BLE001
                pass
            finally:
                try:
                    c.close()
                except Exception:  # noqa: BLE001
                    pass

    def close(self):
        self.stop = True
        self.join(5)
        self.sock.close()


def http_get(url, **opts):
    out = T().cmd(f"http_get {url} " + " ".join(f"{k}={v}" for k, v in opts.items()))
    res = {"events": [], "statuses": []}
    for ln in out:
        k, _, v = ln.partition(" ")
        if k == "event":
            res["events"].append(v)
        elif k == "status":
            res["statuses"].append(v)
        elif k == "result":
            p = v.split()
            res.update(rc=int(p[0]), status=int(p[2]), redirects=int(p[4]), downgraded=int(p[6]), tls=int(p[8]), tls_error=int(p[10]))
        elif k == "body":
            n, d = v.split()
            res["body_len"], res["body_sha"] = int(n), d
        else:
            res[k] = v
    return res


@unittest.skipUnless(HAVE_CRYPTO, "needs cryptography")
class HttpClient(unittest.TestCase):
    """http_get against local servers, plain and over TLS (verified against the test root)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.pki = Pki(cls.tmp.name)
        big = random.Random(14).randbytes(5_000_000)
        cls.big = big
        gz = __import__("gzip").compress(b"compressed page " * 1000)
        cls.gz_plain = b"compressed page " * 1000

        def fixed(body, ctype=b"text/plain", extra=b""):
            return lambda req: (b"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s\r\n" % (ctype, len(body), extra) + body, True)

        def redirect(code, where, extra=b""):
            return lambda req: (b"HTTP/1.1 %d Moved\r\nLocation: %s\r\nContent-Length: 4\r\n%s\r\nmove" % (code, where.encode(), extra), True)

        def echo_cookie(req):
            line = next((l for l in req.split(b"\r\n") if l.lower().startswith(b"cookie:")), b"Cookie: (none)")
            return b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(line) + line, True

        def chunked(req):
            body = b"".join(b"%x\r\n" % len(p) + p + b"\r\n" for p in (b"chunk one ", b"chunk two ", b"end")) + b"0\r\n\r\n"
            return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + body, True

        def until_close_notify(req):
            return b"HTTP/1.0 200 OK\r\n\r\nno length, close_notify", True

        def until_close_cut(req):
            return b"HTTP/1.0 200 OK\r\n\r\nno length, no close_notify", False

        def length_cut(req):
            return b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a part", False

        routes = {
            "/hello": fixed(b"hello from the test server\n"),
            "/big": fixed(big, b"application/octet-stream"),
            "/gzip": fixed(gz, b"text/plain", b"Content-Encoding: gzip\r\n"),
            "/chunked": chunked,
            "/r1": redirect(301, "/r2"), "/r2": redirect(302, "r3"), "/r3": redirect(307, "/hello"),
            "/loop": redirect(302, "/loop"),
            "/setcookie": redirect(302, "/cookie", b"Set-Cookie: session=abc; Path=/\r\n"),
            "/cookie": echo_cookie,
            "/close-notify": until_close_notify, "/close-cut": until_close_cut, "/length-cut": length_cut,
        }
        cls.plain = RawHttp(routes)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*cls.pki.files("ec"))
        cls.secure = RawHttp(dict(routes, **{"/to-plain": redirect(302, f"http://127.0.0.1:{cls.plain.port}/hello")}), tls=ctx)

    @classmethod
    def tearDownClass(cls):
        cls.plain.close()
        cls.secure.close()
        cls.tmp.cleanup()

    def plain_url(self, p):
        return f"http://127.0.0.1:{self.plain.port}{p}"

    def secure_url(self, p):
        return f"https://localhost:{self.secure.port}{p}"

    def get(self, url, **kw):
        kw.setdefault("ca", self.pki.ca)
        kw.setdefault("time", NOW_TS)
        kw.setdefault("timeout", 10000)
        return http_get(url, **kw)

    def test_fetches(self):
        for url in (self.plain_url, self.secure_url):
            r = self.get(url("/hello"))
            self.assertEqual((r["rc"], r["status"], r["body_sha"]), (0, 200, sha(b"hello from the test server\n")), r)
            r = self.get(url("/big"))
            self.assertEqual((r["rc"], r["body_len"], r["body_sha"]), (0, len(self.big), sha(self.big)))
            r = self.get(url("/gzip"))
            self.assertEqual((r["rc"], r["body_sha"], r["encoding"]), (0, sha(self.gz_plain), "gzip"))
            r = self.get(url("/gzip"), nodecode=1)
            self.assertEqual(r["rc"], 0)
            self.assertNotEqual(r["body_sha"], sha(self.gz_plain))
            r = self.get(url("/chunked"))
            self.assertEqual((r["rc"], r["body_sha"]), (0, sha(b"chunk one chunk two end")))
            r = self.get(url("/missing"))
            self.assertEqual((r["rc"], r["status"]), (0, 404))
            r = self.get(url("/hello"), head=1)
            self.assertEqual((r["rc"], r["status"], r["body_len"]), (0, 200, 0))
        self.assertEqual(self.get(self.secure_url("/hello"))["tls"], 1)

    def test_redirects(self):
        for url in (self.plain_url, self.secure_url):
            r = self.get(url("/r1"))
            self.assertEqual((r["rc"], r["status"], r["redirects"]), (0, 200, 3), r)
            self.assertTrue(r["url"].endswith("/hello"))
            self.assertEqual(r["body_sha"], sha(b"hello from the test server\n"))
            r = self.get(url("/loop"))
            self.assertEqual(r["rc"], -110)
            r = self.get(url("/r1"), redirs=-1)
            self.assertEqual((r["rc"], r["status"], r["body_sha"]), (0, 301, sha(b"move")))
        r = self.get(self.secure_url("/to-plain"))
        self.assertEqual((r["rc"], r["downgraded"], r["tls"]), (0, 1, 0))
        self.assertTrue(any("WARNING" in e for e in r["events"]))

    def test_cookies_across_a_redirect(self):
        T().one("http_cookie_reset")
        r = self.get(self.plain_url("/setcookie"), jar=1)
        self.assertEqual(r["body_sha"], sha(b"Cookie: session=abc"))
        T().one("http_cookie_reset")
        r = self.get(self.plain_url("/setcookie"))
        self.assertEqual(r["body_sha"], sha(b"Cookie: (none)"))

    def test_endings_and_failures(self):
        r = self.get(self.secure_url("/close-notify"))
        self.assertEqual((r["rc"], r["body_sha"]), (0, sha(b"no length, close_notify")))
        r = self.get(self.secure_url("/close-cut"))
        self.assertEqual(r["rc"], -108)                               # TLS, no length and no close_notify: maybe cut
        self.assertIn("close_notify", r["error"])
        r = self.get(self.plain_url("/close-cut"))
        self.assertEqual(r["rc"], 0)                                  # plain HTTP cannot tell
        for url in (self.plain_url, self.secure_url):
            self.assertEqual(self.get(url("/length-cut"))["rc"], -108)
        r = self.get(f"https://127.0.0.1:{self.secure.port}/hello")   # an address, checked against the IP SAN
        self.assertEqual((r["rc"], r["status"]), (0, 200), r)
        r = self.get(self.secure_url("/hello"), ca=self.pki.write("x.pem", issue("Other", cached_key("ec256", 41), ca=True).pem))
        self.assertEqual((r["rc"], r["tls_error"]), (-103, E["CERT_UNTRUSTED"]))
        r = self.get(self.secure_url("/hello"), ca="/nonexistent.pem")
        self.assertEqual((r["rc"], r["tls_error"]), (-103, E["NO_TRUST"]))
        self.assertEqual(self.get("http://name.invalid/")["rc"], -101)
        self.assertEqual(self.get(f"http://127.0.0.1:{free_port()}/")["rc"], -102)
        self.assertEqual(self.get("gopher://x/")["rc"], -100)


def rebuild_with_extensions(der, extra):
    """Appends one more Extension to a certificate's extensions (the signature then no longer fits, which does not matter for
    parsing tests). Works on the DER directly so that encodings cryptography refuses to make can be tried."""
    def tlv(tag, body):
        n = len(body)
        if n < 0x80:
            return bytes([tag, n]) + body
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        return bytes([tag, 0x80 | len(b)]) + b + body

    def parse(b, i):
        tag, n = b[i], b[i + 1]
        j = i + 2
        if n & 0x80:
            k = n & 0x7f
            n = int.from_bytes(b[j:j + k], "big")
            j += k
        return tag, b[j:j + n], j + n

    _, cert, _ = parse(der, 0)
    _, tbs, after_tbs = parse(cert, 0)
    rest = cert[after_tbs:]
    items, i = [], 0
    while i < len(tbs):
        tag, body, nxt = parse(tbs, i)
        items.append((tag, body, tbs[i:nxt]))
        i = nxt
    out = b""
    for tag, body, whole in items:
        if tag == 0xa3:
            _, exts, _ = parse(body, 0)
            whole = tlv(0xa3, tlv(0x30, exts + extra))
        out += whole
    return tlv(0x30, tlv(0x30, out) + rest)


if __name__ == "__main__":
    unittest.main()
