"""Known-answer and cross-checking tests for the primitives inside libtls, run on the build machine against Python's
`cryptography` and `hashlib` (see tools/tlshost.py for how the C sources are built into a DLL for this).

Each check_* function returns a list of failure descriptions; an empty list means everything matched."""
import ctypes
import hashlib
import hmac
import os
import random
import struct

import tlslib

P = ctypes.c_char_p
V = ctypes.c_void_p
SZ = ctypes.c_size_t
U64 = ctypes.c_uint64


def _lib():
    L = tlslib.lib()
    if getattr(L, "_kat_ready", False):
        return L
    L.tlsc_hash_once.argtypes = [ctypes.c_int, P, SZ, P]
    L.tlsc_hmac.argtypes = [ctypes.c_int, P, SZ, P, SZ, P]
    L.tlsc_hkdf_extract.argtypes = [ctypes.c_int, P, SZ, P, SZ, P]
    L.tlsc_hkdf_expand.argtypes = [ctypes.c_int, P, SZ, P, SZ, P, SZ]
    L.tlsc_chacha20_xor.argtypes = [P, P, ctypes.c_uint32, P, P, SZ]
    L.tlsc_chacha20poly1305_seal.argtypes = [P, P, P, SZ, P, SZ, P, P]
    L.tlsc_chacha20poly1305_open.argtypes = [P, P, P, SZ, P, SZ, P, P]
    L.tlsc_poly1305.argtypes = [P, P, SZ, P]
    L.tlsc_aes_set_key.argtypes = [V, P, SZ]
    L.tlsc_aes_encrypt_block.argtypes = [V, P, P]
    L.tlsc_aes_force_software.argtypes = [ctypes.c_int]
    L.tlsc_gcm_init.argtypes = [V, P, SZ]
    L.tlsc_gcm_seal.argtypes = [V, P, P, SZ, P, SZ, P, P]
    L.tlsc_gcm_open.argtypes = [V, P, P, SZ, P, SZ, P, P]
    L.tlsc_mont_init.argtypes = [V, V, SZ]
    for name in ("mul", "add", "sub"):
        getattr(L, f"tlsc_mont_{name}").argtypes = [V, V, V, V]
    for name in ("to", "from", "neg", "inv"):
        getattr(L, f"tlsc_mont_{name}").argtypes = [V, V, V]
    L.tlsc_mont_pow.argtypes = [V, V, V, V, SZ]
    L.tlsc_x25519.argtypes = [P, P, P]
    L.tlsc_x25519_base.argtypes = [P, P]
    L.tlsc_ec_size.argtypes = [ctypes.c_int]
    L.tlsc_ec_size.restype = SZ
    L.tlsc_ec_point_valid.argtypes = [ctypes.c_int, P, P]
    L.tlsc_ecdh_keypair.argtypes = [ctypes.c_int, P, P, P]
    L.tlsc_ecdh.argtypes = [ctypes.c_int, P, P, SZ, P]
    L.tlsc_ecdsa_verify.argtypes = [ctypes.c_int, P, SZ, P, SZ, P, SZ, P, SZ]
    L.tlsc_rsa_verify_pkcs1.argtypes = [P, SZ, P, SZ, ctypes.c_int, P, P, SZ]
    L.tlsc_rsa_verify_pss.argtypes = [P, SZ, P, SZ, ctypes.c_int, P, P, SZ, SZ]
    L._kat_ready = True
    return L


def check_hashes():
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
    L, bad = _lib(), []
    algs = {1: ("sha256", 32, hashes.SHA256()), 2: ("sha384", 48, hashes.SHA384()), 3: ("sha512", 64, hashes.SHA512())}
    for alg, (name, hl, hobj) in algs.items():
        for n in list(range(0, 300, 7)) + [1000, 5000]:
            data = os.urandom(n)
            out = ctypes.create_string_buffer(hl)
            L.tlsc_hash_once(alg, data, n, out)
            if out.raw != hashlib.new(name, data).digest():
                bad.append(f"hash {name} {n}")
        for klen in (0, 5, 32, 64, 100, 129, 200):
            key, data = os.urandom(klen), os.urandom(77)
            out = ctypes.create_string_buffer(hl)
            L.tlsc_hmac(alg, key, klen, data, 77, out)
            if out.raw != hmac.new(key, data, name).digest():
                bad.append(f"hmac {name} {klen}")
        salt, ikm, info = os.urandom(20), os.urandom(33), os.urandom(10)
        prk = ctypes.create_string_buffer(hl)
        L.tlsc_hkdf_extract(alg, salt, 20, ikm, 33, prk)
        ref_prk = hmac.new(salt, ikm, name).digest()
        if prk.raw != ref_prk:
            bad.append(f"hkdf extract {name}")
        for ol in (1, 16, hl, hl + 1, 100):
            out = ctypes.create_string_buffer(ol)
            L.tlsc_hkdf_expand(alg, prk.raw, hl, info, 10, out, ol)
            if out.raw != HKDFExpand(hobj, ol, info).derive(ref_prk):
                bad.append(f"hkdf expand {name} {ol}")
    # SHA-256("abc") and the two-block message of FIPS 180-4
    out = ctypes.create_string_buffer(32)
    L.tlsc_hash_once(1, b"abc", 3, out)
    if out.raw.hex() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad":
        bad.append("sha256 abc")
    return bad


def check_chacha_poly():
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    from cryptography.hazmat.primitives.poly1305 import Poly1305
    L, bad = _lib(), []
    for n in list(range(0, 200, 3)) + [255, 256, 257, 1000, 4096, 16384]:
        key, nonce = os.urandom(32), os.urandom(12)
        ctr = struct.unpack("<I", os.urandom(4))[0] & 0x7fffffff
        data = os.urandom(n)
        out = ctypes.create_string_buffer(max(n, 1))
        L.tlsc_chacha20_xor(key, nonce, ctr, data, out, n)
        if out.raw[:n] != Cipher(algorithms.ChaCha20(key, struct.pack("<I", ctr) + nonce), mode=None).encryptor().update(data):
            bad.append(f"chacha20 {n}")
    for n in list(range(0, 130)) + [1000, 4097]:
        key, data = os.urandom(32), os.urandom(n)
        tag = ctypes.create_string_buffer(16)
        L.tlsc_poly1305(key, data, n, tag)
        if tag.raw != Poly1305.generate_tag(key, data):
            bad.append(f"poly1305 {n}")
    for n in (16, 32, 48, 64, 17, 33):                              # carry stress
        for kb in (b"\xff" * 32, b"\x00" * 16 + b"\xff" * 16, b"\xff" * 16 + b"\x00" * 16):
            tag = ctypes.create_string_buffer(16)
            L.tlsc_poly1305(kb, b"\xff" * n, n, tag)
            if tag.raw != Poly1305.generate_tag(kb, b"\xff" * n):
                bad.append(f"poly1305 carry {n}")
    key = bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")
    tag = ctypes.create_string_buffer(16)
    L.tlsc_poly1305(key, b"Cryptographic Forum Research Group", 34, tag)
    if tag.raw.hex() != "a8061dc1305136c6c22b8baf0c0127a9":
        bad.append("poly1305 RFC 8439 vector")
    for n in list(range(0, 100, 5)) + [255, 1000, 16384]:
        for alen in (0, 1, 13, 16, 17, 40):
            key, nonce, aad, pt = os.urandom(32), os.urandom(12), os.urandom(alen), os.urandom(n)
            ct, tag = ctypes.create_string_buffer(max(n, 1)), ctypes.create_string_buffer(16)
            L.tlsc_chacha20poly1305_seal(key, nonce, aad, alen, pt, n, ct, tag)
            ref = ChaCha20Poly1305(key).encrypt(nonce, pt, aad)
            if ct.raw[:n] + tag.raw != ref:
                bad.append(f"aead seal {n} {alen}")
                continue
            out = ctypes.create_string_buffer(max(n, 1))
            if L.tlsc_chacha20poly1305_open(key, nonce, aad, alen, ref[:n], n, out, ref[n:]) != 0 or out.raw[:n] != pt:
                bad.append(f"aead open {n} {alen}")
            t2 = bytearray(ref[n:]); t2[3] ^= 1
            if L.tlsc_chacha20poly1305_open(key, nonce, aad, alen, ref[:n], n, out, bytes(t2)) == 0:
                bad.append("aead accepted a bad tag")
            if n:
                c2 = bytearray(ref[:n]); c2[n // 2] ^= 0x80
                if L.tlsc_chacha20poly1305_open(key, nonce, aad, alen, bytes(c2), n, out, ref[n:]) == 0:
                    bad.append("aead accepted bad ciphertext")
    return bad


def check_aes_gcm():
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    L, bad = _lib(), []
    modes_run = []
    for forced in (0, 1):
        L.tlsc_aes_force_software(forced)
        modes_run.append("software" if forced or not L.tlsc_aes_hw_available() else "aes-ni")
        for klen in (16, 32):
            for _ in range(20):
                key, blk = os.urandom(klen), os.urandom(16)
                kbuf, out = ctypes.create_string_buffer(512), ctypes.create_string_buffer(16)
                L.tlsc_aes_set_key(kbuf, key, klen)
                L.tlsc_aes_encrypt_block(kbuf, blk, out)
                if out.raw != Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(blk):
                    bad.append(f"aes {klen} forced={forced}")
        for key, ct in ((bytes(range(16)), "69c4e0d86a7b0430d8cdb78070b4c55a"), (bytes(range(32)), "8ea2b7ca516745bfeafc49904b496089")):
            kbuf, out = ctypes.create_string_buffer(512), ctypes.create_string_buffer(16)
            L.tlsc_aes_set_key(kbuf, key, len(key))
            L.tlsc_aes_encrypt_block(kbuf, bytes.fromhex("00112233445566778899aabbccddeeff"), out)
            if out.raw.hex() != ct:
                bad.append(f"FIPS 197 vector {len(key)}")
        for klen in (16, 32):
            for n in list(range(0, 80, 3)) + [255, 256, 1000, 16384]:
                for alen in (0, 5, 16, 20, 33):
                    key, iv, aad, pt = os.urandom(klen), os.urandom(12), os.urandom(alen), os.urandom(n)
                    g, ct, tag = ctypes.create_string_buffer(512), ctypes.create_string_buffer(max(n, 1)), ctypes.create_string_buffer(16)
                    L.tlsc_gcm_init(g, key, klen)
                    L.tlsc_gcm_seal(g, iv, aad, alen, pt, n, ct, tag)
                    ref = AESGCM(key).encrypt(iv, pt, aad)
                    if ct.raw[:n] + tag.raw != ref:
                        bad.append(f"gcm seal {klen} {n} {alen} forced={forced}")
                        continue
                    out = ctypes.create_string_buffer(max(n, 1))
                    if L.tlsc_gcm_open(g, iv, aad, alen, ref[:n], n, out, ref[n:]) != 0 or out.raw[:n] != pt:
                        bad.append(f"gcm open {klen} {n} {alen}")
                    t2 = bytearray(ref[n:]); t2[15] ^= 1
                    if L.tlsc_gcm_open(g, iv, aad, alen, ref[:n], n, out, bytes(t2)) == 0:
                        bad.append("gcm accepted a bad tag")
    L.tlsc_aes_force_software(0)
    return bad, modes_run


def _limbs(v, n):
    return (U64 * n)(*[(v >> (64 * i)) & ((1 << 64) - 1) for i in range(n)])


def _int(arr, n):
    return sum(int(arr[i]) << (64 * i) for i in range(n))


def _is_prime(n, k=20):
    if n < 4:
        return n in (2, 3)
    if n % 2 == 0:
        return False
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(k):
        a = random.randrange(2, n - 1)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def check_bignum():
    L, bad = _lib(), []
    for bits in (64, 128, 192, 256, 384, 512, 1024, 2048, 3072, 4096):
        n = bits // 64
        for _ in range(3 if bits > 1024 else 8):
            if bits <= 512:
                while True:
                    m = random.getrandbits(bits) | (1 << (bits - 1)) | 1
                    if _is_prime(m):
                        break
            else:
                m = random.getrandbits(bits) | (1 << (bits - 1)) | 1
            c = ctypes.create_string_buffer(2048)
            if L.tlsc_mont_init(c, _limbs(m, n), n) != 0:
                bad.append(f"mont_init {bits}")
                continue
            R = 1 << (64 * n)
            a, b = random.randrange(m), random.randrange(m)
            am, bm, r = (U64 * n)(), (U64 * n)(), (U64 * n)()
            L.tlsc_mont_to(c, am, _limbs(a, n)); L.tlsc_mont_to(c, bm, _limbs(b, n))
            if _int(am, n) != a * R % m:
                bad.append(f"to {bits}")
            for name, want in (("mul", a * b % m), ("add", (a + b) % m), ("sub", (a - b) % m)):
                getattr(L, f"tlsc_mont_{name}")(c, r, am, bm)
                L.tlsc_mont_from(c, r, r)
                if _int(r, n) != want:
                    bad.append(f"{name} {bits}")
            L.tlsc_mont_neg(c, r, am); L.tlsc_mont_from(c, r, r)
            if _int(r, n) != (-a) % m:
                bad.append(f"neg {bits}")
            e = random.getrandbits(min(bits, 130))
            L.tlsc_mont_pow(c, r, am, _limbs(e, 3), 3); L.tlsc_mont_from(c, r, r)
            if _int(r, n) != pow(a, e, m):
                bad.append(f"pow {bits}")
            if bits <= 512 and a:
                L.tlsc_mont_inv(c, r, am); L.tlsc_mont_from(c, r, r)
                if _int(r, n) != pow(a, -1, m):
                    bad.append(f"inv {bits}")
            for a2 in (0, 1, m - 1):
                for b2 in (0, 1, m - 1):
                    L.tlsc_mont_to(c, am, _limbs(a2, n)); L.tlsc_mont_to(c, bm, _limbs(b2, n))
                    L.tlsc_mont_mul(c, r, am, bm); L.tlsc_mont_from(c, r, r)
                    if _int(r, n) != a2 * b2 % m:
                        bad.append(f"mul extreme {bits}")
                    L.tlsc_mont_add(c, r, am, bm); L.tlsc_mont_from(c, r, r)
                    if _int(r, n) != (a2 + b2) % m:
                        bad.append(f"add extreme {bits}")
    return bad


def check_x25519():
    from cryptography.hazmat.primitives.asymmetric import x25519
    L, bad = _lib(), []
    vec = [("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
           ("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957")]
    for k, u, want in vec:
        out = ctypes.create_string_buffer(32)
        if L.tlsc_x25519(out, bytes.fromhex(k), bytes.fromhex(u)) != 0 or out.raw.hex() != want:
            bad.append("x25519 RFC 7748 vector")
    k = u = bytes([9]) + bytes(31)
    for _ in range(1000):                                           # RFC 7748 section 5.2, 1000 iterations
        out = ctypes.create_string_buffer(32)
        L.tlsc_x25519(out, k, u)
        u, k = k, out.raw
    if k.hex() != "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51":
        bad.append("x25519 1000 iterations")
    for _ in range(30):
        priv, peer = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
        out = ctypes.create_string_buffer(32)
        if L.tlsc_x25519(out, priv.private_bytes_raw(), peer.public_key().public_bytes_raw()) != 0 or out.raw != priv.exchange(peer.public_key()):
            bad.append("x25519 exchange")
        L.tlsc_x25519_base(out, priv.private_bytes_raw())
        if out.raw != priv.public_key().public_bytes_raw():
            bad.append("x25519 base point")
    for low in ("00" * 32, "01" + "00" * 31, "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
                "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157", "ecff" + "ff" * 29 + "7f", "edff" + "ff" * 29 + "7f", "eeff" + "ff" * 29 + "7f"):
        out = ctypes.create_string_buffer(32)
        if L.tlsc_x25519(out, os.urandom(32), bytes.fromhex(low)) == 0:
            bad.append("x25519 accepted a low-order point")
    return bad


def check_ec():
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    L, bad = _lib(), []
    curves = {1: (ec.SECP256R1(), 32, 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551),
              2: (ec.SECP384R1(), 48, 0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973)}
    X962 = (serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    for cid, (cv, size, nord) in curves.items():
        for _ in range(10):
            priv, pub = ctypes.create_string_buffer(size), ctypes.create_string_buffer(1 + 2 * size)
            if L.tlsc_ecdh_keypair(cid, os.urandom(2 * size), priv, pub) != 0:
                bad.append("keypair"); continue
            ref = ec.derive_private_key(int.from_bytes(priv.raw, "big"), cv)
            if pub.raw != ref.public_key().public_bytes(*X962):
                bad.append(f"keypair public key {cid}")
            peer = ec.generate_private_key(cv)
            peerpub = peer.public_key().public_bytes(*X962)
            shared = ctypes.create_string_buffer(size)
            if L.tlsc_ecdh(cid, priv.raw, peerpub, len(peerpub), shared) != 0 or shared.raw != ref.exchange(ec.ECDH(), peer.public_key()):
                bad.append(f"ecdh {cid}")
            bp = bytearray(peerpub); bp[-1] ^= 1
            if L.tlsc_ecdh(cid, priv.raw, bytes(bp), len(bp), shared) == 0:
                bad.append("ecdh accepted an off-curve point")
            if L.tlsc_ecdh(cid, priv.raw, bytes([2]) + peerpub[1:1 + size], 1 + size, shared) == 0:
                bad.append("compressed point accepted")
        if L.tlsc_ec_point_valid(cid, b"\xff" * size, b"\xff" * size) or L.tlsc_ec_point_valid(cid, bytes(size), bytes(size)):
            bad.append("invalid coordinates accepted")
        gen = ec.derive_private_key(1, cv).public_key().public_bytes(*X962)
        for k in (1, 2, 3, 15, 16, 17, nord - 1, nord - 2, 1 << (8 * size - 1), 0xff << (8 * size - 8)):
            shared = ctypes.create_string_buffer(size)
            rc = L.tlsc_ecdh(cid, (k % (1 << (8 * size))).to_bytes(size, "big"), gen, len(gen), shared)
            if k % nord and (rc != 0 or shared.raw != ec.derive_private_key(k % nord, cv).public_key().public_numbers().x.to_bytes(size, "big")):
                bad.append(f"scalar {hex(k)[:12]} {cid}")
        if L.tlsc_ecdh(cid, bytes(size), gen, len(gen), ctypes.create_string_buffer(size)) == 0 or \
           L.tlsc_ecdh(cid, nord.to_bytes(size, "big"), gen, len(gen), ctypes.create_string_buffer(size)) == 0:
            bad.append("k = 0 or k = n accepted")
        for hname, hobj, halg in (("sha256", hashes.SHA256(), 1), ("sha384", hashes.SHA384(), 2), ("sha512", hashes.SHA512(), 3)):
            for _ in range(5):
                key = ec.generate_private_key(cv)
                pub = key.public_key().public_bytes(*X962)
                msg = os.urandom(random.randrange(0, 300))
                r, s = utils.decode_dss_signature(key.sign(msg, ec.ECDSA(hobj)))
                dig = hashlib.new(hname, msg).digest()
                rb, sb = r.to_bytes((r.bit_length() + 7) // 8 or 1, "big"), s.to_bytes((s.bit_length() + 7) // 8 or 1, "big")
                if L.tlsc_ecdsa_verify(cid, pub, len(pub), dig, len(dig), rb, len(rb), sb, len(sb)) != 0:
                    bad.append(f"ecdsa valid signature rejected {cid} {hname}"); continue
                d2 = bytearray(dig); d2[0] ^= 1
                r2 = bytearray(rb); r2[-1] ^= 1
                s2 = bytearray(sb); s2[-1] ^= 4
                other = ec.generate_private_key(cv).public_key().public_bytes(*X962)
                for what, args in (("hash", (pub, bytes(d2), rb, sb)), ("r", (pub, dig, bytes(r2), sb)), ("s", (pub, dig, rb, bytes(s2))), ("key", (other, dig, rb, sb)),
                                   ("r=0", (pub, dig, bytes(size), sb)), ("s=0", (pub, dig, rb, bytes(size))), ("r=n", (pub, dig, nord.to_bytes(size, "big"), sb)),
                                   ("s=n", (pub, dig, rb, nord.to_bytes(size, "big")))):
                    p_, d_, r_, s_ = args
                    if L.tlsc_ecdsa_verify(cid, p_, len(p_), d_, len(d_), r_, len(r_), s_, len(s_)) == 0:
                        bad.append(f"ecdsa accepted a bad {what}")
    return bad


def check_rsa(sizes=(1024, 2048, 3072)):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    L, bad = _lib(), []
    H = {"sha256": (hashes.SHA256(), 1), "sha384": (hashes.SHA384(), 2), "sha512": (hashes.SHA512(), 3)}
    for bits in sizes:
        key = rsa.generate_private_key(65537, bits)
        pn = key.public_key().public_numbers()
        nb, eb = pn.n.to_bytes(bits // 8, "big"), pn.e.to_bytes((pn.e.bit_length() + 7) // 8, "big")
        for hname, (hobj, halg) in H.items():
            msg = os.urandom(100)
            dig = hashlib.new(hname, msg).digest()
            sig = key.sign(msg, padding.PKCS1v15(), hobj)
            if L.tlsc_rsa_verify_pkcs1(nb, len(nb), eb, len(eb), halg, dig, sig, len(sig)) != 0:
                bad.append(f"pkcs1 valid rejected {bits} {hname}")
            d2 = bytearray(dig); d2[5] ^= 1
            s2 = bytearray(sig); s2[10] ^= 1
            if L.tlsc_rsa_verify_pkcs1(nb, len(nb), eb, len(eb), halg, bytes(d2), sig, len(sig)) == 0:
                bad.append("pkcs1 wrong hash accepted")
            if L.tlsc_rsa_verify_pkcs1(nb, len(nb), eb, len(eb), halg, dig, bytes(s2), len(s2)) == 0:
                bad.append("pkcs1 wrong signature accepted")
            if L.tlsc_rsa_verify_pkcs1(nb, len(nb), eb, len(eb), halg, dig, sig[1:], len(sig) - 1) == 0:
                bad.append("pkcs1 short signature accepted")
            nb0 = b"\x00" + nb
            if L.tlsc_rsa_verify_pkcs1(nb0, len(nb0), eb, len(eb), halg, dig, sig, len(sig)) != 0:
                bad.append("zero-prefixed modulus rejected")
            for salt in (len(dig), 0, 20):
                if bits // 8 - 1 < len(dig) + salt + 2:
                    continue
                sigp = key.sign(msg, padding.PSS(mgf=padding.MGF1(hobj), salt_length=salt), hobj)
                if L.tlsc_rsa_verify_pss(nb, len(nb), eb, len(eb), halg, dig, sigp, len(sigp), salt) != 0:
                    bad.append(f"pss valid rejected {bits} {hname} {salt}")
                if L.tlsc_rsa_verify_pss(nb, len(nb), eb, len(eb), halg, bytes(d2), sigp, len(sigp), salt) == 0:
                    bad.append("pss wrong hash accepted")
                sp2 = bytearray(sigp); sp2[7] ^= 0x10
                if L.tlsc_rsa_verify_pss(nb, len(nb), eb, len(eb), halg, dig, bytes(sp2), len(sp2), salt) == 0:
                    bad.append("pss wrong signature accepted")
                if L.tlsc_rsa_verify_pss(nb, len(nb), eb, len(eb), halg, dig, sigp, len(sigp), salt + 1) == 0:
                    bad.append("pss wrong salt length accepted")
            if L.tlsc_rsa_verify_pss(nb, len(nb), eb, len(eb), halg, dig, sig, len(sig), len(dig)) == 0:
                bad.append("pkcs1 signature accepted as pss")
    key = rsa.generate_private_key(3, 2048)
    nb = key.public_key().public_numbers().n.to_bytes(256, "big")
    dig = hashlib.sha256(b"hello").digest()
    sig = key.sign(b"hello", padding.PKCS1v15(), hashes.SHA256())
    if L.tlsc_rsa_verify_pkcs1(nb, 256, b"\x03", 1, 1, dig, sig, 256) != 0:
        bad.append("e = 3 rejected")
    if L.tlsc_rsa_verify_pkcs1(nb, 256, b"\x02", 1, 1, dig, sig, 256) == 0 or L.tlsc_rsa_verify_pkcs1(nb, 256, b"\x01", 1, 1, dig, sig, 256) == 0:
        bad.append("even exponent or e = 1 accepted")
    return bad
