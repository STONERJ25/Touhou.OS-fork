#!/usr/bin/env python3
"""Host tests for the part of the IPv6 stack that needs no machine (kernel/net/ip6_proto.c): the text form of addresses (against the C library's inet_ntop, which is what
RFC 5952 plus the mixed notation of RFC 4291 comes to in practice), prefix arithmetic and the derived addresses (against Python's ipaddress and hand-made values), the
pseudo-header checksum (against the Internet checksum of RFC 1071 computed in big-endian arithmetic), the walker over the extension headers (packets built here with struct:
every verdict, every pointer, the limits, and 20000 random byte strings that must never make it read outside the packet), the option parsers, and the reassembly queue
(random fragmentations in random order, duplicates, every kind of overlap). The C file is built as a shared library with gcc (UBSan on, -Werror) and driven through ctypes;
every expectation is computed here, in Python, from the RFCs, never from the C code.

    python tools/hosttest.py discover -s tests/host -p "test_net6.py" -v
"""
import ctypes
import ipaddress
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-net6"


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    so = OUT / "ip6_proto.so"
    deps = [REPO / "kernel" / "net" / "ip6_proto.c", REPO / "kernel" / "include" / "net6.h", REPO / "kernel" / "include" / "types.h"]
    if so.exists() and all(d.stat().st_mtime <= so.stat().st_mtime for d in deps):
        return so
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(REPO / "kernel" / "include"), str(deps[0]), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of ip6_proto.c failed:\n" + r.stdout + r.stderr)
    return so


def build_libc_inet():
    """user/libc/src/inet.c as a shared library whose functions carry another name, so that it does not meet the C library's own (the C library is what it is compared with)."""
    so = OUT / "libc_inet.so"
    src = REPO / "user" / "libc" / "src" / "inet.c"
    if so.exists() and src.stat().st_mtime <= so.stat().st_mtime:
        return so
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    renames = ["inet_aton", "inet_addr", "inet_ntoa", "inet_pton", "inet_ntop", "in6addr_any", "in6addr_loopback"]
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
           *[f"-D{n}=gaptest_{n}" for n in renames], str(src), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of inet.c failed:\n" + r.stdout + r.stderr)
    return so


class Walk(ctypes.Structure):
    _fields_ = [("verdict", ctypes.c_int), ("pp_code", ctypes.c_uint8), ("pp_ptr", ctypes.c_uint32), ("proto", ctypes.c_uint8), ("uoff", ctypes.c_uint32),
                ("nh_ptr", ctypes.c_uint32), ("nexthdrs", ctypes.c_int), ("frag_off", ctypes.c_uint32), ("frag_more", ctypes.c_bool), ("frag_id", ctypes.c_uint32),
                ("frag_nh", ctypes.c_uint8), ("frag_hoff", ctypes.c_uint32)]


ND_MAX_PREFIX, ND_MAX_RDNSS, R6_MAX_FRAGS = 8, 4, 64


class NdOpts(ctypes.Structure):
    _fields_ = [("slla", ctypes.c_void_p), ("tlla", ctypes.c_void_p), ("prefix", ctypes.c_void_p * ND_MAX_PREFIX), ("nprefix", ctypes.c_int), ("mtu", ctypes.c_void_p),
                ("rdnss", ctypes.c_void_p * ND_MAX_RDNSS), ("nrdnss", ctypes.c_int)]


class Reasm(ctypes.Structure):
    _fields_ = [("buf", ctypes.c_void_p), ("total", ctypes.c_uint32), ("got", ctypes.c_uint32), ("nfrag", ctypes.c_int), ("start", ctypes.c_uint32 * R6_MAX_FRAGS),
                ("end", ctypes.c_uint32 * R6_MAX_FRAGS), ("have_first", ctypes.c_bool), ("first_nh", ctypes.c_uint8)]


def lib():
    L = ctypes.CDLL(str(build()))
    c = ctypes
    L.in6_scope.argtypes = [c.c_char_p]
    L.in6_prefix_eq.argtypes = [c.c_char_p, c.c_char_p, c.c_int]
    L.in6_prefix_eq.restype = c.c_bool
    L.in6_common_prefix.argtypes = [c.c_char_p, c.c_char_p]
    L.in6_mask.argtypes = [c.c_char_p, c.c_char_p, c.c_int]
    L.in6_solicited_node.argtypes = [c.c_char_p, c.c_char_p]
    L.in6_is_solicited_node.argtypes = [c.c_char_p]
    L.in6_is_solicited_node.restype = c.c_bool
    L.in6_mcast_mac.argtypes = [c.c_char_p, c.c_char_p]
    L.in6_eui64.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p]
    L.in6_str.argtypes = [c.c_char_p, c.c_char_p, c.c_size_t]
    L.in6_str.restype = c.c_size_t
    L.in6_hex.argtypes = [c.c_char_p, c.c_char_p]
    L.net6_pseudo_csum.argtypes = [c.c_char_p, c.c_char_p, c.c_uint8, c.c_uint32]
    L.net6_pseudo_csum.restype = c.c_uint32
    L.ip6_walk.argtypes = [c.c_char_p, c.c_size_t, c.c_uint8, c.c_uint32, c.c_bool, c.c_bool, c.c_bool, c.POINTER(Walk)]
    L.nd_opts_parse.argtypes = [c.c_char_p, c.c_size_t, c.POINTER(NdOpts)]
    L.nd_opts_parse.restype = c.c_bool
    L.ip6_opts_check.argtypes = [c.c_char_p, c.c_size_t, c.c_bool, c.POINTER(c.c_uint8), c.POINTER(c.c_uint32)]
    L.ip6_opts_check.restype = c.c_int
    L.reasm6_add.argtypes = [c.POINTER(Reasm), c.c_uint32, c.c_bool, c.c_uint8, c.c_char_p, c.c_uint32]
    L.reasm6_add.restype = c.c_int
    return L


L = None


def setUpModule():
    global L
    L = lib()


def a6(text):
    return ipaddress.IPv6Address(text).packed


def text_of(raw):
    buf = ctypes.create_string_buffer(64)
    n = L.in6_str(raw, buf, 64)
    assert n == len(buf.value), (n, buf.value)
    return buf.value.decode()


class Addresses(unittest.TestCase):
    CASES = ["::", "::1", "fe80::1", "fe80::5054:ff:fe12:3456", "2001:db8::", "2001:db8:0:0:1:0:0:1", "2001:db8:0:1:1:1:1:1", "1:0:0:2:0:0:0:3", "0:0:1:0:0:0:0:0",
             "ff02::1:ff12:3456", "::ffff:10.0.2.2", "::ffff:0:0", "::1.2.3.4", "::10.0.2.15", "1::", "1:2:3:4:5:6:7:8", "fec0::2", "fec0::3", "2001:db8:aaaa:bbbb:cccc:dddd:eeee:ffff",
             "0:0:0:0:0:ffff:ffff:ffff", "::2", "::100", "0:1::", "1:0:0:0:0:0:0:1", "1:0:0:2:3:0:0:4"]

    def test_text_form_agrees_with_the_c_library(self):
        for case in self.CASES:
            raw = a6(case)
            self.assertEqual(text_of(raw), socket.inet_ntop(socket.AF_INET6, raw), case)

    def test_text_form_of_random_addresses_agrees(self):
        rnd = random.Random(6)
        for _ in range(30000):
            words = [rnd.choice([0, 0, 0, 1, 0xFFFF, rnd.getrandbits(16)]) for _ in range(8)]
            if rnd.random() < 0.2:
                words[:6] = [0] * 5 + [rnd.choice([0, 0xFFFF])]
            raw = b"".join(struct.pack(">H", w) for w in words)
            self.assertEqual(text_of(raw), socket.inet_ntop(socket.AF_INET6, raw), raw.hex())

    def test_text_form_is_what_rfc_5952_asks_for(self):
        self.assertEqual(text_of(a6("2001:0db8:0000:0000:0001:0000:0000:0001")), "2001:db8::1:0:0:1")      # the first of two runs of equal length
        self.assertEqual(text_of(a6("2001:db8:0:1:1:1:1:1")), "2001:db8:0:1:1:1:1:1")                    # a single zero group is not abbreviated
        self.assertEqual(text_of(a6("FE80:0:0:0:0:0:0:1")), "fe80::1")
        self.assertEqual(text_of(a6("::ffff:10.0.2.2")), "::ffff:10.0.2.2")

    def test_truncation_never_overruns(self):
        raw = a6("fe80::5054:ff:fe12:3456")
        for cap in range(0, 30):
            buf = ctypes.create_string_buffer(b"\xAA" * 40, 40)
            n = L.in6_str(raw, buf, cap)
            self.assertEqual(n, len("fe80::5054:ff:fe12:3456"))
            self.assertEqual(buf.raw[max(cap, 0):], b"\xAA" * (40 - cap) if cap else b"\xAA" * 40)
            if cap:
                self.assertEqual(buf.value.decode(), "fe80::5054:ff:fe12:3456"[:cap - 1])

    def test_hex_form_of_proc_net_if_inet6(self):
        buf = ctypes.create_string_buffer(33)
        L.in6_hex(a6("fe80::5054:ff:fe12:3456"), buf)
        self.assertEqual(buf.value.decode(), "fe800000000000005054" + "00fffe123456")

    def test_prefix_arithmetic_against_ipaddress(self):
        rnd = random.Random(7)
        for _ in range(4000):
            a = bytes(rnd.getrandbits(8) for _ in range(16))
            b = bytearray(a)
            if rnd.random() < 0.8:
                bit = rnd.randrange(128)
                b[bit // 8] ^= 0x80 >> (bit % 8)
            b = bytes(b)
            plen = rnd.randrange(0, 129)
            ia, ib = int.from_bytes(a, "big"), int.from_bytes(b, "big")
            same = plen == 0 or (ia >> (128 - plen)) == (ib >> (128 - plen))
            self.assertEqual(L.in6_prefix_eq(a, b, plen), same, (a.hex(), b.hex(), plen))
            common = 128 if ia == ib else 128 - (ia ^ ib).bit_length()
            self.assertEqual(L.in6_common_prefix(a, b), common)
            out = ctypes.create_string_buffer(16)
            L.in6_mask(out, a, plen)
            net = ipaddress.IPv6Network((a, plen), strict=False)
            self.assertEqual(out.raw, net.network_address.packed)
        self.assertTrue(L.in6_prefix_eq(a6("fe80::1"), a6("fe80::2"), 64))
        self.assertFalse(L.in6_prefix_eq(a6("fe80::1"), a6("fe81::1"), 16))

    def test_scopes(self):
        for text, scope in [("::1", 1), ("fe80::1", 2), ("febf::1", 2), ("fec0::2", 5), ("2001:db8::1", 14), ("fd00::1", 14), ("ff02::1", 2), ("ff01::1", 1), ("ff0e::1", 14), ("ff05::1", 5),
                            ("::ffff:1.2.3.4", 14)]:
            self.assertEqual(L.in6_scope(a6(text)), scope, text)

    def test_derived_addresses(self):
        out = ctypes.create_string_buffer(16)
        L.in6_solicited_node(out, a6("fe80::5054:ff:fe12:3456"))
        self.assertEqual(out.raw, a6("ff02::1:ff12:3456"))
        self.assertTrue(L.in6_is_solicited_node(out.raw))
        self.assertFalse(L.in6_is_solicited_node(a6("ff02::1:fe12:3456")))
        self.assertFalse(L.in6_is_solicited_node(a6("ff02::2:ff12:3456")))
        self.assertFalse(L.in6_is_solicited_node(a6("ff05::1:ff12:3456")))
        mac = ctypes.create_string_buffer(6)
        L.in6_mcast_mac(mac, a6("ff02::1:ff12:3456"))
        self.assertEqual(mac.raw, bytes.fromhex("3333ff123456"))
        L.in6_mcast_mac(mac, a6("ff02::1"))
        self.assertEqual(mac.raw, bytes.fromhex("333300000001"))
        L.in6_eui64(out, a6("fe80::"), bytes.fromhex("525400123456"))
        self.assertEqual(out.raw, a6("fe80::5054:ff:fe12:3456"))
        L.in6_eui64(out, a6("2001:db8:1:2::"), bytes.fromhex("0200000000aa"))                          # the universal/local bit flips both ways
        self.assertEqual(out.raw, a6("2001:db8:1:2:0:ff:fe00:aa"))
        L.in6_eui64(out, a6("fec0::"), bytes.fromhex("f89a25cb1234"))
        self.assertEqual(out.raw, a6("fec0::fa9a:25ff:fecb:1234"))


class LibcAddresses(unittest.TestCase):
    """inet_pton and inet_ntop of the project's C library against the machine's: the same answers for what is accepted, what is refused and how it is written."""

    @classmethod
    def setUpClass(cls):
        cls.lib = ctypes.CDLL(str(build_libc_inet()))
        cls.lib.gaptest_inet_pton.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.gaptest_inet_ntop.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        cls.lib.gaptest_inet_ntop.restype = ctypes.c_char_p

    def pton(self, af, text):
        out = ctypes.create_string_buffer(16)
        r = self.lib.gaptest_inet_pton(af, text.encode(), out)
        return out.raw[:16 if af == socket.AF_INET6 else 4] if r == 1 else None

    def theirs(self, af, text):
        try:
            return socket.inet_pton(af, text)
        except (OSError, ValueError, UnicodeError):
            return None

    def test_the_forms_of_rfc_4291(self):
        for text in ["::", "::1", "1::", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1::8", "FE80::1", "fe80::5054:ff:fe12:3456", "::ffff:1.2.3.4", "::1.2.3.4",
                     "1:2:3:4:5:6:1.2.3.4", "64:ff9b::192.0.2.33", "0000:0000:0000:0000:0000:0000:0000:0001", "2001:db8::", "ff02::1:ff12:3456", "fec0::2"]:
            self.assertEqual(self.pton(socket.AF_INET6, text), self.theirs(socket.AF_INET6, text), text)
            self.assertIsNotNone(self.pton(socket.AF_INET6, text), text)
        for text in ["", ":", ":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", "::1::", "1::2::3", ":1:2:3:4:5:6:7", "1:2:3:4:5:6:7:", "12345::", "g::", "::ffff:1.2.3", "::ffff:1.2.3.256",
                     "::ffff:01.2.3.4", "1.2.3.4", "::1.2.3.4.5", "1.2.3.4::", "::1:1.2.3.4:5", " ::1", "::1 ", "::1%eth0", "1:2:3:4:5:6:7::8", "::-1", "0x1::"]:
            self.assertIsNone(self.pton(socket.AF_INET6, text), text)
            self.assertIsNone(self.theirs(socket.AF_INET6, text), text)

    def test_random_text_is_judged_like_the_c_library(self):
        rnd = random.Random(13)
        alphabet = "0123456789abcdefABCDEF:::..xg%- "
        for _ in range(40000):
            kind = rnd.random()
            if kind < 0.5:
                groups = [format(rnd.choice([0, 0, 1, 0xFFFF, rnd.getrandbits(16)]), rnd.choice(["x", "X", "04x"])) for _ in range(rnd.randrange(1, 10))]
                text = ":".join(groups)
                if rnd.random() < 0.6:
                    i = rnd.randrange(len(groups))
                    j = min(len(groups), i + rnd.randrange(1, 4))
                    text = ":".join(groups[:i]) + "::" + ":".join(groups[j:])
                if rnd.random() < 0.25:
                    text += ":" + ".".join(str(rnd.choice([0, 1, 10, 255, rnd.randrange(300)])) for _ in range(4))
            else:
                text = "".join(rnd.choice(alphabet) for _ in range(rnd.randrange(0, 20)))
            if rnd.random() < 0.3 and text:                                          # a typo
                k = rnd.randrange(len(text))
                text = text[:k] + rnd.choice(alphabet) + text[k + rnd.randrange(0, 2):]
            self.assertEqual(self.pton(socket.AF_INET6, text), self.theirs(socket.AF_INET6, text), repr(text))

    def test_ipv4_text_is_judged_like_the_c_library(self):
        rnd = random.Random(14)
        for _ in range(20000):
            parts = [str(rnd.choice([0, 1, 9, 10, 99, 100, 255, 256, 300, rnd.randrange(1000)])) for _ in range(rnd.choice([3, 4, 4, 4, 5]))]
            text = ".".join(parts)
            if rnd.random() < 0.2:
                text = rnd.choice(["0" + parts[0], " " + text, text + " ", text + ".", "." + text, "+" + text])
            self.assertEqual(self.pton(socket.AF_INET, text), self.theirs(socket.AF_INET, text), repr(text))

    def test_writing_agrees_with_the_c_library(self):
        rnd = random.Random(15)
        for _ in range(30000):
            words = [rnd.choice([0, 0, 0, 1, 0xFFFF, rnd.getrandbits(16)]) for _ in range(8)]
            if rnd.random() < 0.2:
                words[:6] = [0] * 5 + [rnd.choice([0, 0xFFFF])]
            raw = b"".join(struct.pack(">H", w) for w in words)
            buf = ctypes.create_string_buffer(64)
            r = self.lib.gaptest_inet_ntop(socket.AF_INET6, raw, buf, 64)
            self.assertEqual(r.decode(), socket.inet_ntop(socket.AF_INET6, raw), raw.hex())
        raw = bytes([10, 0, 2, 15])
        self.assertEqual(self.lib.gaptest_inet_ntop(socket.AF_INET, raw, ctypes.create_string_buffer(16), 16), b"10.0.2.15")

    def test_a_buffer_that_is_too_small_is_refused(self):
        raw = a6("fe80::5054:ff:fe12:3456")
        for size in range(1, len("fe80::5054:ff:fe12:3456") + 1):
            buf = ctypes.create_string_buffer(64)
            self.assertIsNone(self.lib.gaptest_inet_ntop(socket.AF_INET6, raw, buf, size), size)
        self.assertIsNotNone(self.lib.gaptest_inet_ntop(socket.AF_INET6, raw, ctypes.create_string_buffer(64), len("fe80::5054:ff:fe12:3456") + 1))

    def test_unknown_family(self):
        out = ctypes.create_string_buffer(16)
        self.assertEqual(self.lib.gaptest_inet_pton(99, b"::1", out), -1)


def internet_checksum(data):
    """RFC 1071 in big-endian arithmetic."""
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f">{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def le_sum(data):
    """What net_csum() does with the bytes: the sum of little-endian words (a trailing byte counts as a low byte), end-around carries."""
    s = 0
    for i in range(0, len(data) - 1, 2):
        s += data[i] | data[i + 1] << 8
    if len(data) % 2:
        s += data[-1]
    return s


def fold16(s):
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


class Checksums(unittest.TestCase):
    def test_pseudo_header_matches_rfc_8200(self):
        rnd = random.Random(8)
        for _ in range(3000):
            src, dst = bytes(rnd.getrandbits(8) for _ in range(16)), bytes(rnd.getrandbits(8) for _ in range(16))
            proto = rnd.choice([6, 17, 58])
            msg = bytearray(rnd.getrandbits(8) for _ in range(rnd.randrange(20, 400)))
            csum_off = {6: 16, 17: 6, 58: 2}[proto]
            msg[csum_off:csum_off + 2] = b"\0\0"
            pseudo = src + dst + struct.pack(">I", len(msg)) + b"\0\0\0" + bytes([proto])
            want = internet_checksum(pseudo + bytes(msg))
            start = L.net6_pseudo_csum(src, dst, proto, len(msg))
            got = fold16(start + le_sum(bytes(msg)))                                # exactly net_csum(msg, n, start)
            stored = ~got & 0xFFFF                                                  # stored with a native 16-bit write
            self.assertEqual(struct.pack("<H", stored), struct.pack(">H", want))
            # a message that carries its checksum sums to 0xFFFF
            full = bytearray(msg)
            full[csum_off:csum_off + 2] = struct.pack(">H", want)
            self.assertEqual(fold16(start + le_sum(bytes(full))), 0xFFFF)

    def test_long_length_uses_the_full_32_bits(self):
        src, dst = a6("::1"), a6("::1")
        n = 70000
        pseudo = src + dst + struct.pack(">I", n) + b"\0\0\0" + bytes([6])
        # the sum of the pseudo-header alone, big-endian, folded, must equal the folded little-endian one byte-swapped
        be = fold16(sum(struct.unpack(">20H", pseudo)))
        le = fold16(L.net6_pseudo_csum(src, dst, 6, n))
        self.assertEqual(((le & 0xFF) << 8) | (le >> 8), be)


def ip6_header(plen, nh, src=None, dst=None):
    return struct.pack(">IHBB", 0x60000000, plen, nh, 64) + (src or a6("fe80::1")) + (dst or a6("fe80::2"))


def pad_opts(n):
    """n bytes of padding options (n >= 2: PadN; 1: Pad1)."""
    return b"\0" if n == 1 else bytes([1, n - 2]) + b"\0" * (n - 2) if n else b""


def ext(nh, body, hdrlen_units=None):
    """A hop-by-hop / destination options style header: next header, length in 8-byte units minus one, body (padded to a multiple of 8 including the two bytes)."""
    total = 2 + len(body)
    pad = (-total) % 8
    body = body + pad_opts(pad)
    return bytes([nh, (2 + len(body)) // 8 - 1]) + body


def walk(pkt, nh=None, off=40, first=True, allow_frag=True, mcast=False):
    w = Walk()
    L.ip6_walk(pkt, len(pkt), pkt[6] if nh is None else nh, off, first, allow_frag, mcast, ctypes.byref(w))
    return w


class Walker(unittest.TestCase):
    def packet(self, first_nh, chain, payload=b"\0" * 8):
        return ip6_header(len(chain) + len(payload), first_nh) + chain + payload

    def test_plain_upper_layers(self):
        for proto in (6, 17, 58):
            pkt = self.packet(proto, b"", b"\0" * 20)
            w = walk(pkt)
            self.assertEqual((w.verdict, w.proto, w.uoff, w.nh_ptr, w.nexthdrs), (0, proto, 40, 6, 0))

    def test_no_next_header_and_unknown_protocols_are_reported_to_the_caller(self):
        w = walk(self.packet(59, b"", b"junk after the end"))
        self.assertEqual((w.verdict, w.proto, w.uoff), (0, 59, 40))                       # RFC 8200 section 4.7: what follows is ignored
        for proto in (50, 51, 132, 255):
            w = walk(self.packet(proto, b""))
            self.assertEqual((w.verdict, w.proto, w.uoff, w.nh_ptr), (0, proto, 40, 6))       # the caller sends the parameter problem for a protocol it does not know

    def test_chain_of_extension_headers(self):
        hbh = ext(60, b"")                                                 # hop-by-hop, then destination options
        dst = ext(17, b"\x05\x02\x00\x00")                                  # a router alert option inside (skipped)
        pkt = self.packet(0, hbh + dst, b"\0" * 12)
        w = walk(pkt)
        self.assertEqual((w.verdict, w.proto, w.uoff, w.nexthdrs), (0, 17, 40 + len(hbh) + len(dst), 2))
        self.assertEqual(w.nh_ptr, 40 + len(hbh))                                       # the byte of the destination options header that said 17

    def test_hop_by_hop_only_first(self):
        chain = ext(0, b"") + ext(17, b"")                                   # destination options, then a hop-by-hop header after it
        pkt = self.packet(60, chain)
        w = walk(pkt)
        self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 1, 40))                    # the Next Header byte of the destination options header that said 0
        pkt = self.packet(0, ext(0, b"") + ext(17, b""))
        w = walk(pkt)
        self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 1, 40))
        w = walk(self.packet(17, b""), nh=0, off=40, first=False)                         # after reassembly: not allowed at all
        self.assertEqual(w.verdict, 2)

    def test_option_actions(self):
        for t, mcast, want in [(0x3E, False, 0), (0x5E, False, 1), (0x9E, False, 2), (0xDE, False, 2), (0xDE, True, 1), (0x9E, True, 2), (0xC2, False, 2), (0x05, False, 0), (0x04, False, 0)]:
            body = bytes([t, 2, 0, 0, 1, 0])                                  # an option of two data bytes, then padding to 8
            chain = bytes([17, 0]) + body
            pkt = self.packet(60, chain)
            w = walk(pkt, mcast=mcast)
            if want == 0:
                self.assertEqual((w.verdict, w.proto), (0, 17), hex(t))
            elif want == 1:
                self.assertEqual(w.verdict, 1, hex(t))
            else:
                self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 2, 42), hex(t))     # the pointer is the option's type byte

    def test_malformed_options(self):
        chain = bytes([17, 0, 0x3E, 9, 0, 0, 0, 0])                           # an option that claims 9 data bytes inside a header that holds 4
        w = walk(self.packet(60, chain))
        self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 0, 43))                    # the pointer is the length byte
        chain = bytes([17, 0, 1, 0, 0, 0, 0, 0x3E])                           # a type byte at the very end, with no length
        w = walk(self.packet(60, chain))
        self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 0, 47))

    def test_routing_header(self):
        for rtype in (0, 2, 4, 99):
            seg_left = bytes([17, 0, rtype, 0, 0, 0, 0, 0])                 # segments left 0: skipped whatever the type
            w = walk(self.packet(43, seg_left))
            self.assertEqual((w.verdict, w.proto, w.uoff), (0, 17, 48), rtype)
            seg_left = bytes([17, 0, rtype, 2, 0, 0, 0, 0])
            w = walk(self.packet(43, seg_left))
            self.assertEqual((w.verdict, w.pp_code, w.pp_ptr), (2, 0, 42), rtype)         # RFC 5095: type 0 with segments left; and no type is implemented

    def test_truncated_headers_are_dropped(self):
        chain = ext(17, b"")
        pkt = self.packet(60, chain, b"")
        for cut in range(0, len(chain)):
            part = pkt[:40 + cut]
            w = Walk()
            L.ip6_walk(part, len(part), 60, 40, True, True, False, ctypes.byref(w))
            self.assertEqual(w.verdict, 1, cut)
        w = Walk()                                                                       # a header whose length reaches beyond the packet
        big = bytes([17, 5]) + b"\0" * 6
        part = ip6_header(len(big), 60) + big
        L.ip6_walk(part, len(part), 60, 40, True, True, False, ctypes.byref(w))
        self.assertEqual(w.verdict, 1)

    def test_fragment_header(self):
        frag = struct.pack(">BBHI", 17, 0, (185 << 3) | 1, 0xDEADBEEF)
        pkt = self.packet(44, frag, b"X" * 64)
        w = walk(pkt)
        self.assertEqual((w.verdict, w.proto, w.uoff, w.frag_off, w.frag_more, w.frag_id, w.frag_nh, w.frag_hoff), (0, 44, 48, 185 * 8, True, 0xDEADBEEF, 17, 40))
        frag = struct.pack(">BBHI", 6, 0, 0, 7)
        w = walk(self.packet(44, frag, b"Y" * 8))
        self.assertEqual((w.frag_off, w.frag_more, w.frag_nh), (0, False, 6))
        frag2 = struct.pack(">BBHI", 44, 0, 8, 1) + struct.pack(">BBHI", 17, 0, 0, 2)    # a second fragment header: no (RFC 8200: one per packet)
        w = walk(self.packet(44, frag2), allow_frag=True)
        self.assertEqual(w.verdict, 0)                                                       # the first stops the walk; the caller walks the reassembled data with allow_frag off
        w = walk(self.packet(17, b""), nh=44, off=40, first=False, allow_frag=False)
        self.assertEqual(w.verdict, 1)

    def test_more_than_eight_headers_is_dropped(self):
        for n, ok in [(8, True), (9, False)]:
            chain = b"".join(ext(60 if i < n - 1 else 17, b"") for i in range(n))
            pkt = self.packet(60, chain)
            w = walk(pkt)
            self.assertEqual(w.verdict == 0, ok, n)

    def test_random_bytes_never_escape_the_packet(self):
        rnd = random.Random(9)
        for i in range(20000):
            n = rnd.randrange(0, 200)
            data = bytearray(rnd.getrandbits(8) for _ in range(n))
            if n >= 40 and rnd.random() < 0.8:                                   # shape it a little: a header and plausible chain bytes
                data[0] = 0x60
                data[6] = rnd.choice([0, 43, 44, 60, 17, 6, 58, 59, 50, rnd.getrandbits(8)])
                for k in range(40, n - 1, rnd.choice([2, 8, 8, 16])):
                    data[k] = rnd.choice([0, 43, 44, 60, 17, 6, 58, 59, rnd.getrandbits(8)])
                    data[k + 1] = rnd.choice([0, 0, 1, 2, rnd.getrandbits(8) & 15])
            raw = bytes(data)
            w = Walk()
            L.ip6_walk(raw, len(raw), raw[6] if n > 6 else 0, 40, True, True, bool(rnd.getrandbits(1)), ctypes.byref(w))
            self.assertIn(w.verdict, (0, 1, 2))
            if w.verdict == 0:
                self.assertLessEqual(w.uoff, len(raw))
                self.assertLess(w.nexthdrs, 9)
            if w.verdict == 2:
                self.assertLess(w.pp_ptr, len(raw), raw.hex())
                self.assertLessEqual(w.pp_code, 2)


class Options(unittest.TestCase):
    def check(self, body, mcast=False):
        code, ptr = ctypes.c_uint8(0), ctypes.c_uint32(0)
        v = L.ip6_opts_check(body, len(body), mcast, ctypes.byref(code), ctypes.byref(ptr))
        return v, code.value, ptr.value

    def test_padding_and_known_options(self):
        self.assertEqual(self.check(b"\0\0\0\0\0\0")[0], 0)
        self.assertEqual(self.check(bytes([1, 4, 0, 0, 0, 0]))[0], 0)
        self.assertEqual(self.check(bytes([5, 2, 0, 0, 1, 0]))[0], 0)
        self.assertEqual(self.check(b"")[0], 0)

    def test_the_first_bad_option_decides(self):
        self.assertEqual(self.check(bytes([0x3E, 0, 0x9E, 0, 0, 0])), (2, 2, 2))          # an option to skip, then one that wants a parameter problem (pointer: its type byte)
        self.assertEqual(self.check(bytes([0x5E, 0, 0x9E, 0]))[0], 1)                     # the discard comes first


class NdOptions(unittest.TestCase):
    def parse(self, data):
        o = NdOpts()
        ok = L.nd_opts_parse(data, len(data), ctypes.byref(o))
        return ok, o

    def test_the_usual_router_advertisement_options(self):
        mac = bytes.fromhex("525500000002")
        prefix = struct.pack(">BBBBIII", 3, 4, 64, 0xC0, 86400, 14400, 0) + a6("fec0::")
        mtu = struct.pack(">BBHI", 5, 1, 0, 1500)
        rdnss = struct.pack(">BBHI", 25, 3, 0, 600) + a6("fec0::3")
        data = bytes([1, 1]) + mac + prefix + mtu + rdnss
        ok, o = self.parse(data)
        self.assertTrue(ok)
        self.assertEqual(ctypes.string_at(o.slla, 6), mac)
        self.assertIsNone(o.tlla)
        self.assertEqual((o.nprefix, o.nrdnss), (1, 1))
        self.assertEqual(ctypes.string_at(o.prefix[0], 32), prefix)
        self.assertEqual(ctypes.string_at(o.mtu, 8), mtu)
        self.assertEqual(ctypes.string_at(o.rdnss[0], 24), rdnss)

    def test_zero_length_and_overrun_discard_the_message(self):
        self.assertFalse(self.parse(bytes([1, 0, 0, 0, 0, 0, 0, 0]))[0])
        self.assertFalse(self.parse(bytes([1, 2, 0, 0, 0, 0, 0, 0]))[0])                  # claims 16 bytes, has 8
        self.assertFalse(self.parse(bytes([1]))[0])
        self.assertTrue(self.parse(b"")[0])

    def test_unknown_options_are_skipped_and_odd_sized_known_ones_ignored(self):
        data = bytes([200, 1, 0, 0, 0, 0, 0, 0]) + bytes([3, 2]) + b"\0" * 14 + bytes([25, 2]) + b"\0" * 14 + bytes([5, 1]) + b"\0" * 6
        ok, o = self.parse(data)
        self.assertTrue(ok)
        self.assertEqual((o.nprefix, o.nrdnss), (0, 0))                                      # wrong lengths: ignored, not fatal
        self.assertIsNotNone(o.mtu)

    def test_bounds(self):
        prefix = struct.pack(">BBBBIII", 3, 4, 64, 0xC0, 86400, 14400, 0) + a6("fec0::")
        ok, o = self.parse(prefix * 20)
        self.assertTrue(ok)
        self.assertEqual(o.nprefix, ND_MAX_PREFIX)

    def test_random_options_never_overrun(self):
        rnd = random.Random(10)
        for _ in range(20000):
            n = rnd.randrange(0, 120)
            data = bytes(rnd.choice([1, 2, 3, 4, 5, 25, 0, rnd.getrandbits(8)]) if i % 8 < 2 and rnd.random() < 0.7 else rnd.getrandbits(8) for i in range(n))
            ok, o = self.parse(data)
            if ok:
                for ptr_ in list(o.prefix)[:o.nprefix] + list(o.rdnss)[:o.nrdnss]:
                    self.assertIsNotNone(ptr_)


def fragments(payload, sizes):
    out, off = [], 0
    for i, s in enumerate(sizes):
        last = i == len(sizes) - 1
        out.append((off, not last, payload[off:off + s]))
        off += s
    assert off == len(payload)
    return out


class Reassembly(unittest.TestCase):
    def new(self):
        buf = ctypes.create_string_buffer(40 + 65535)
        q = Reasm()
        ctypes.memset(ctypes.byref(q), 0, ctypes.sizeof(q))
        q.buf = ctypes.addressof(buf)
        return q, buf

    def add(self, q, off, more, data, nh=17):
        return L.reasm6_add(ctypes.byref(q), off, more, nh, bytes(data), len(data))

    def test_in_order(self):
        payload = bytes(range(256)) * 10
        q, buf = self.new()
        frs = fragments(payload, [1232, 1232, len(payload) - 2464])
        for i, (off, more, data) in enumerate(frs):
            r = self.add(q, off, more, data)
            self.assertEqual(r, 1 if i == len(frs) - 1 else 0)
        self.assertEqual(buf.raw[40:40 + len(payload)], payload)
        self.assertEqual((q.total, q.first_nh), (len(payload), 17))

    def test_random_orders_and_duplicates(self):
        rnd = random.Random(11)
        for _ in range(300):
            n = rnd.randrange(8, 12000)
            payload = bytes(rnd.getrandbits(8) for _ in range(n))
            sizes, left = [], n
            while left > 8:                                                          # fragments of 200 to 1600 bytes: well inside the 64 a datagram may have
                s = min(left - 1, 8 * rnd.randrange(25, 200))
                s = max(8, s - s % 8)
                if s >= left:
                    break
                sizes.append(s)
                left -= s
            sizes.append(left)
            frs = fragments(payload, sizes)
            order = list(range(len(frs)))
            rnd.shuffle(order)
            q, buf = self.new()
            done = False
            for k, idx in enumerate(order):
                off, more, data = frs[idx]
                r = self.add(q, off, more, data, nh=6 if off == 0 else 99)
                if rnd.random() < 0.3:                                              # the network repeats a fragment: only the copy is dropped
                    self.assertEqual(self.add(q, off, more, data), 2)
                if r == 1:
                    done = True
                    self.assertEqual(k, len(order) - 1)
            self.assertTrue(done, (n, sizes))
            self.assertEqual(buf.raw[40:40 + n], payload)
            self.assertEqual(q.first_nh, 6)

    def test_the_duplicate_of_a_complete_datagram_is_harmless(self):
        q, buf = self.new()
        self.assertEqual(self.add(q, 0, True, b"a" * 16), 0)
        self.assertEqual(self.add(q, 0, True, b"a" * 16), 2)
        self.assertEqual(self.add(q, 16, False, b"b" * 10), 1)

    def test_every_overlap_discards_the_datagram(self):
        cases = [((0, True, 16), (8, True, 16)),                         # partial overlap at the end
                 ((8, True, 16), (0, True, 16)),                         # partial overlap at the start
                 ((0, True, 32), (8, True, 8)),                          # contained
                 ((8, True, 8), (0, True, 32)),                          # containing
                 ((0, True, 16), (0, True, 8)),                          # same start, shorter
                 ((0, True, 8), (0, True, 16)),                          # same start, longer
                 ((16, False, 8), (8, True, 16))]                        # runs over the last fragment
        for (a, b) in cases:
            q, _ = self.new()
            self.assertIn(self.add(q, a[0], a[1], b"x" * a[2]), (0, 1))
            self.assertEqual(self.add(q, b[0], b[1], b"y" * b[2]), 3, (a, b))

    def test_invalid_fragments(self):
        q, _ = self.new()
        self.assertEqual(self.add(q, 0, True, b""), 3)                    # nothing in it
        q, _ = self.new()
        self.assertEqual(self.add(q, 0, True, b"x" * 12), 3)              # not a multiple of 8, and more to come
        q, _ = self.new()
        self.assertEqual(self.add(q, 4, True, b"x" * 8), 3)               # an offset that is not a multiple of 8
        q, _ = self.new()
        self.assertEqual(self.add(q, 65528, False, b"x" * 7), 0)          # exactly 65535 bytes: allowed
        q, _ = self.new()
        self.assertEqual(self.add(q, 65528, False, b"x" * 8), 3)          # one more: not
        q, _ = self.new()
        self.assertEqual(self.add(q, 16, False, b"x" * 8), 0)
        self.assertEqual(self.add(q, 0, True, b"x" * 32), 3)              # beyond the end the last fragment announced
        q, _ = self.new()
        self.assertEqual(self.add(q, 0, True, b"x" * 8), 0)
        self.assertEqual(self.add(q, 8, False, b"x" * 8), 1)
        self.assertEqual(self.add(q, 8, False, b"x" * 9), 3)              # a second, different last fragment

    def test_two_last_fragments(self):
        q, _ = self.new()
        self.assertEqual(self.add(q, 8, False, b"x" * 8), 0)
        self.assertEqual(self.add(q, 16, False, b"x" * 8), 3)

    def test_incomplete_with_a_hole_never_completes(self):
        q, _ = self.new()
        self.assertEqual(self.add(q, 0, True, b"x" * 8), 0)
        self.assertEqual(self.add(q, 16, False, b"x" * 8), 0)             # [8, 16) is missing

    def test_fragment_count_is_bounded(self):
        q, _ = self.new()
        for i in range(R6_MAX_FRAGS):
            self.assertEqual(self.add(q, 8 * i, True, b"x" * 8), 0)
        self.assertEqual(self.add(q, 8 * R6_MAX_FRAGS, True, b"x" * 8), 3)

    def test_random_fragment_soup_never_corrupts_memory(self):
        rnd = random.Random(12)
        for _ in range(500):
            q, buf = self.new()
            for _ in range(rnd.randrange(1, 90)):
                off = 8 * rnd.randrange(0, 8200)
                ln = rnd.choice([0, 1, 7, 8, 16, 1232, 8 * rnd.randrange(1, 300), rnd.randrange(0, 70000)])
                r = self.add(q, off, bool(rnd.getrandbits(1)), b"z" * min(ln, 70000))
                self.assertIn(r, (0, 1, 2, 3))
                if r in (1, 3):
                    break
            self.assertLessEqual(q.nfrag, R6_MAX_FRAGS)


class NativeAbi(unittest.TestCase):
    """shared/netabi.h repeats Linux's numbers and structure layouts for IPv6, which is what lets the Linux personality hand AF_INET6 sockets to the native handlers and
    what makes the /proc files readable by Linux programs. Every number and every field offset is compared with the C library's and the kernel headers' of the machine this
    runs on: one program that includes only netabi.h and one that includes only the system's headers print the same lines (both generated from the same table)."""

    # the system's headers that define them (some of Linux's headers cannot be included together), and (our name, the system's expression) for each
    SOCKET_H = "#include <sys/socket.h>\n#include <netinet/in.h>"
    GROUPS = [
        (SOCKET_H, [(n, n) for n in ("AF_INET6", "PF_INET6", "IPPROTO_IPV6", "IPPROTO_ICMPV6", "IPV6_CHECKSUM", "IPV6_UNICAST_HOPS", "IPV6_MULTICAST_IF", "IPV6_MULTICAST_HOPS",
                                     "IPV6_MULTICAST_LOOP", "IPV6_MTU_DISCOVER", "IPV6_MTU", "IPV6_RECVERR", "IPV6_V6ONLY", "IPV6_RECVPKTINFO", "IPV6_PKTINFO",
                                     "IPV6_RECVHOPLIMIT", "IPV6_HOPLIMIT", "IPV6_DONTFRAG", "IPV6_RECVTCLASS", "IPV6_TCLASS", "MSG_TRUNC")]),
        ("#include <linux/if_addr.h>", [("G6A_TEMPORARY", "IFA_F_TEMPORARY"), ("G6A_DADFAILED", "IFA_F_DADFAILED"), ("G6A_DEPRECATED", "IFA_F_DEPRECATED"),
                                        ("G6A_TENTATIVE", "IFA_F_TENTATIVE"), ("G6A_PERMANENT", "IFA_F_PERMANENT")]),
        ("#include <linux/neighbour.h>", [("G6N_INCOMPLETE", "NUD_INCOMPLETE"), ("G6N_REACHABLE", "NUD_REACHABLE"), ("G6N_STALE", "NUD_STALE"), ("G6N_DELAY", "NUD_DELAY"),
                                          ("G6N_PROBE", "NUD_PROBE"), ("G6N_PERMANENT", "NUD_PERMANENT")]),
        ("#include <net/route.h>", [("G6R_UP", "RTF_UP"), ("G6R_GATEWAY", "RTF_GATEWAY"), ("G6R_HOST", "RTF_HOST")]),
        ("#include <linux/ipv6_route.h>", [("G6R_DEFAULT", "RTF_DEFAULT"), ("G6R_ADDRCONF", "RTF_ADDRCONF"), ("G6R_EXPIRES", "RTF_EXPIRES")]),
    ]
    FIELDS = [("family", "sin6_family"), ("port", "sin6_port"), ("flowinfo", "sin6_flowinfo"), ("addr", "sin6_addr"), ("scope_id", "sin6_scope_id")]

    def run_c(self, includes, lines):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no C compiler")
        import tempfile
        with tempfile.TemporaryDirectory(prefix="net6abi-") as tmp:
            src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
            src.write_text("#define _GNU_SOURCE\n#include <stdio.h>\n#include <stddef.h>\n" + includes + "\nint main(void) {\n" + "\n".join(lines) + "\n    return 0;\n}\n", encoding="utf-8")
            r = subprocess.run([cc, "-w", "-I", str(REPO / "shared"), "-o", str(exe), str(src)], capture_output=True, text=True)
            if r.returncode != 0:
                self.skipTest("this machine's headers do not compile the check: " + r.stderr[-300:])
            return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout

    def test_numbers_and_layout_are_linuxs(self):
        for includes, pairs in self.GROUPS:
            ours = [f'    printf("{n} %lu\\n", (unsigned long)({n}));' for n, _ in pairs]
            theirs = [f'    printf("{n} %lu\\n", (unsigned long)({t}));' for n, t in pairs]
            if includes == self.SOCKET_H:                                       # (the layout of sockaddr_in6 is in the same header)
                ours.append('    printf("size %zu\\n", sizeof(struct gsockaddr_in6));')
                theirs.append('    printf("size %zu\\n", sizeof(struct sockaddr_in6));')
                for f, t in self.FIELDS:
                    ours.append(f'    printf("offset {f} %zu\\n", offsetof(struct gsockaddr_in6, {f}));')
                    theirs.append(f'    printf("offset {f} %zu\\n", offsetof(struct sockaddr_in6, {t}));')
            a = self.run_c('#include "netabi.h"', ours)
            b = self.run_c(includes, theirs)
            self.assertEqual(a, b, includes)
            self.assertGreaterEqual(len(a.splitlines()), len(pairs))
            if includes == self.SOCKET_H:
                self.assertIn("size 28", a)


if __name__ == "__main__":
    unittest.main()
