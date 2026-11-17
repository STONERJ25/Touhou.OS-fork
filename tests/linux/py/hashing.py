"""The hashlib, hmac, secrets and base64 modules: every digest of the standard algorithms on known inputs (the OpenSSL-backed ones and the built-in ones), incremental updates, large
inputs, key derivation, message authentication codes and encodings."""
import base64
import binascii
import hashlib
import hmac
import io
import secrets
import zlib


def show(label, value=""):
    print(f"{label}: {value}")


print("--- digests of fixed inputs")
for name in ("md5", "sha1", "sha224", "sha256", "sha384", "sha512", "sha512_224", "sha512_256", "sha3_224", "sha3_256", "sha3_384", "sha3_512", "blake2b", "blake2s"):
    for data in (b"", b"abc", b"The quick brown fox jumps over the lazy dog"):
        h = hashlib.new(name, data)
        show(f"{name}({data[:3]!r}...)", h.hexdigest())
    show(f"{name} sizes", (h.digest_size, h.block_size, h.name))
show("shake_128", hashlib.shake_128(b"abc").hexdigest(32))
show("shake_256", hashlib.shake_256(b"abc").hexdigest(48))

print("--- constructors, updates and copies")
show("constructor", hashlib.sha256(b"hello").hexdigest() == hashlib.new("sha256", b"hello").hexdigest())
h = hashlib.sha256()
for part in (b"he", b"l", b"lo"):
    h.update(part)
show("incremental", h.hexdigest() == hashlib.sha256(b"hello").hexdigest())
h2 = h.copy()
h2.update(b" world")
show("copy", (h.hexdigest() == hashlib.sha256(b"hello").hexdigest(), h2.hexdigest() == hashlib.sha256(b"hello world").hexdigest()))
show("digest and hexdigest", (hashlib.md5(b"x").digest().hex() == hashlib.md5(b"x").hexdigest(), len(hashlib.sha512(b"x").digest())))
show("a million a's", (hashlib.sha1(b"a" * 1000000).hexdigest(), hashlib.sha256(b"a" * 1000000).hexdigest()))
show("sha3 of a million a's", hashlib.sha3_256(b"a" * 1000000).hexdigest())
show("blake2b of 10 MB", hashlib.blake2b(bytes(range(256)) * 40960, digest_size=20).hexdigest())
show("memoryview and bytearray", (hashlib.md5(memoryview(b"abc")).hexdigest(), hashlib.md5(bytearray(b"abc")).hexdigest()))
try:
    hashlib.md5("text")
except TypeError as e:
    show("strings are refused", e)
try:
    hashlib.new("no-such-algorithm")
except ValueError as e:
    show("unknown algorithm", e)
show("algorithms", ("sha256" in hashlib.algorithms_guaranteed, "md5" in hashlib.algorithms_available, sorted(hashlib.algorithms_guaranteed)))
buf = io.BytesIO(bytes(range(256)) * 100)
show("file_digest", hashlib.file_digest(buf, "sha256").hexdigest())
show("blake2 with a key and salt", hashlib.blake2b(b"data", key=b"secret", salt=b"salty", person=b"me", digest_size=16).hexdigest())
show("blake2s tree parameters", hashlib.blake2s(b"data", fanout=2, depth=2, leaf_size=64, node_depth=1, last_node=True).hexdigest())
show("usedforsecurity", hashlib.md5(b"abc", usedforsecurity=False).hexdigest())

print("--- key derivation and codes")
show("pbkdf2_hmac", hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 1000, 32).hex())
show("pbkdf2_hmac sha1", hashlib.pbkdf2_hmac("sha1", b"password", b"salt", 1, 20).hex())
show("scrypt", hashlib.scrypt(b"password", salt=b"NaCl", n=1024, r=8, p=1, dklen=32).hex())
show("hmac sha256", hmac.new(b"key", b"The quick brown fox jumps over the lazy dog", "sha256").hexdigest())
show("hmac md5", hmac.new(b"key", b"The quick brown fox jumps over the lazy dog", "md5").hexdigest())
show("hmac.digest", hmac.digest(b"key", b"message", "sha512").hex()[:32])
m = hmac.new(b"key", digestmod="sha1")
m.update(b"in ")
m.update(b"pieces")
show("hmac incremental", (m.hexdigest() == hmac.new(b"key", b"in pieces", "sha1").hexdigest(), m.digest_size, m.name))
show("compare_digest", (hmac.compare_digest(b"abc", b"abc"), hmac.compare_digest(b"abc", b"abd"), hmac.compare_digest("abc", "abc")))
show("long key", hmac.new(b"k" * 200, b"msg", "sha256").hexdigest())

print("--- encodings and checksums")
data = bytes(range(256))
show("base64", (base64.b64encode(data[:20]), base64.b64decode(b"AAECAwQFBgcICQ=="), base64.urlsafe_b64encode(b"\xfb\xff\xfe")))
show("base32/16/85", (base64.b32encode(b"hello"), base64.b16encode(b"hello"), base64.a85encode(b"hello"), base64.b85encode(b"hello")))
show("base64 round trip of 1 MB", base64.b64decode(base64.b64encode(data * 4096)) == data * 4096)
show("binascii", (binascii.hexlify(b"\x01\xab"), binascii.unhexlify("01ab"), binascii.crc32(b"hello"), binascii.b2a_uu(b"hi")))
show("zlib checksums", (zlib.crc32(b"hello world"), zlib.adler32(b"hello world"), zlib.crc32(b"a" * 100000)))
show("crc32 incremental", zlib.crc32(b" world", zlib.crc32(b"hello")) == zlib.crc32(b"hello world"))

print("--- randomness")
show("lengths", (len(secrets.token_bytes(16)), len(secrets.token_hex(8)), len(secrets.token_urlsafe(12)), 0 <= secrets.randbelow(10) < 10))
show("two draws differ", secrets.token_bytes(16) != secrets.token_bytes(16))
show("choice", secrets.choice("abc") in "abc")
show("compare", secrets.compare_digest("same", "same"))
print("--- done")
