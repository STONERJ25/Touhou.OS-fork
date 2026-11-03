#!/usr/bin/env python3
"""Host tests for the part of the kernel's network time client that needs no machine (kernel/net/ntp_proto.c): the NTP packet and its
checks, the offset formulas, the era arithmetic around 2036, the "host:port,..." lists, the DNS messages, and the POSIX time zones that
ntp.rtc=1 uses. The C file is built as a shared library with gcc (UBSan on, -Werror) and driven through ctypes; every expectation is
computed here, in Python, from the RFCs (4330, 1035) and the POSIX text, never from the C code.

    python tools/hosttest.py discover -s tests/host -p "test_ntp.py" -v
"""
import ctypes
import datetime
import os
import random
import shutil
import struct
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-ntp"
NTP_DELTA = 2208988800


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    so = OUT / "ntp_proto.so"
    deps = [REPO / "kernel" / "net" / "ntp_proto.c", REPO / "kernel" / "include" / "ntp.h", REPO / "kernel" / "include" / "types.h"]
    if so.exists() and all(d.stat().st_mtime <= so.stat().st_mtime for d in deps):
        return so
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(REPO / "kernel" / "include"), str(deps[0]), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of ntp_proto.c failed:\n" + r.stdout + r.stderr)
    return so


class Reply(ctypes.Structure):
    _fields_ = [("li", ctypes.c_int), ("version", ctypes.c_int), ("stratum", ctypes.c_int), ("t2_ms", ctypes.c_int64),
                ("t3_ms", ctypes.c_int64), ("root_ms", ctypes.c_int64), ("kiss", ctypes.c_char * 5)]


class Spec(ctypes.Structure):
    _fields_ = [("host", ctypes.c_char * 64), ("port", ctypes.c_uint16)]


class Rule(ctypes.Structure):
    _fields_ = [("month", ctypes.c_int), ("week", ctypes.c_int), ("wday", ctypes.c_int), ("secs", ctypes.c_int)]


class Zone(ctypes.Structure):
    _fields_ = [("std_off", ctypes.c_int32), ("dst_off", ctypes.c_int32), ("has_dst", ctypes.c_bool), ("start", Rule), ("end", Rule)]


def lib():
    so = build()
    L = ctypes.CDLL(str(so))
    u8p = ctypes.c_char_p
    L.ntp_unix_ms_to_ts.argtypes = [ctypes.c_int64, ctypes.c_char * 8]
    L.ntp_ts_to_unix_ms.argtypes = [u8p]
    L.ntp_ts_to_unix_ms.restype = ctypes.c_int64
    L.ntp_make_request.argtypes = [ctypes.c_char * 48, ctypes.c_int64, ctypes.c_uint16]
    L.ntp_parse_reply.argtypes = [u8p, ctypes.c_size_t, u8p, ctypes.POINTER(Reply)]
    L.ntp_strerror.restype = ctypes.c_char_p
    L.ntp_offset_delay.argtypes = [ctypes.c_int64] * 4 + [ctypes.POINTER(ctypes.c_int64)] * 2
    L.ntp_delay_ok.argtypes = [ctypes.c_int64]
    L.ntp_delay_ok.restype = ctypes.c_bool
    L.ntp_agree.argtypes = [ctypes.c_int64, ctypes.c_int64]
    L.ntp_agree.restype = ctypes.c_bool
    L.ntp_next_spec.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(Spec)]
    L.ntp_next_spec.restype = ctypes.c_bool
    L.ntp_parse_ip4.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32)]
    L.ntp_parse_ip4.restype = ctypes.c_bool
    L.ntp_dns_query.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint16, ctypes.c_char_p]
    L.ntp_dns_query.restype = ctypes.c_size_t
    L.ntp_dns_answers.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint16, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    L.ntp_dns_answers.restype = ctypes.c_int
    L.ntp_zone_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(Zone)]
    L.ntp_zone_parse.restype = ctypes.c_bool
    L.ntp_zone_offset.argtypes = [ctypes.POINTER(Zone), ctypes.c_int64]
    L.ntp_zone_offset.restype = ctypes.c_int32
    return L


L = None


def setUpModule():
    global L
    L = lib()


# the C enum, repeated here on purpose: a renumbering must be noticed
OK, E_SHORT, E_MODE, E_VERSION, E_ORIGIN, E_KISS, E_STRATUM, E_ALARM, E_TIMES, E_ROOT = range(10)


def ts_bytes(unix_ms):
    """A 64-bit NTP timestamp from Unix milliseconds, from the definition in RFC 4330 section 3."""
    sec, ms = divmod(unix_ms, 1000)
    return struct.pack(">II", (sec + NTP_DELTA) & 0xFFFFFFFF, (ms << 32) // 1000)


def reply(sent, t2_ms, t3_ms, li=0, vn=4, mode=4, stratum=2, refid=b"\x0a\x00\x00\x01", root_delay=0, root_disp=0, origin=None):
    p = bytearray(48)
    p[0] = li << 6 | vn << 3 | mode
    p[1] = stratum
    struct.pack_into(">i", p, 4, root_delay)
    struct.pack_into(">I", p, 8, root_disp)
    p[12:16] = refid
    p[24:32] = sent if origin is None else origin
    p[32:40] = ts_bytes(t2_ms) if t2_ms is not None else bytes(8)
    p[40:48] = ts_bytes(t3_ms) if t3_ms is not None else bytes(8)
    return bytes(p)


T0 = 1_793_000_000_000          # a Unix millisecond time in 2026, well after the 2020 floor
SENT = bytes.fromhex("edbd5f4300112233")


def parse(pkt, sent=SENT):
    r = Reply()
    code = L.ntp_parse_reply(pkt, len(pkt), sent, ctypes.byref(r))
    return code, r


class Timestamps(unittest.TestCase):
    def conv(self, ms):
        out = ctypes.create_string_buffer(8)
        L.ntp_unix_ms_to_ts(ms, out)
        return out.raw

    def test_encode_matches_the_definition(self):
        rnd = random.Random(4330)
        for ms in [0, 1, 999, 1000, T0, T0 + 1, 1_000_000_000_123, 2_085_978_495_999, 2_085_978_496_000, 4_000_000_000_000] + [rnd.randrange(0, 4_000_000_000_000) for _ in range(300)]:
            self.assertEqual(self.conv(ms), ts_bytes(ms), ms)

    def test_roundtrip_to_the_millisecond(self):
        rnd = random.Random(1)
        for ms in [T0, T0 + 1, T0 + 999] + [rnd.randrange(-1_000_000_000, 4_000_000_000_000) for _ in range(500)]:
            ts = self.conv(ms)
            self.assertEqual(L.ntp_ts_to_unix_ms(ts), ms)              # (a timestamp made from whole milliseconds comes back as the same milliseconds)

    def test_decoding_rounds_to_the_nearest_millisecond(self):
        # a server's fraction is not a whole number of milliseconds: .0004 s is 0 ms, .0005 s and .9996 s round up (and the last one carries into the second)
        def frac(seconds):
            return struct.pack(">II", NTP_DELTA + 1_000_000_000, int(seconds * 2**32))
        self.assertEqual(L.ntp_ts_to_unix_ms(frac(0.0004)), 1_000_000_000_000)
        self.assertEqual(L.ntp_ts_to_unix_ms(frac(0.0006)), 1_000_000_000_001)
        self.assertEqual(L.ntp_ts_to_unix_ms(frac(0.9996)), 1_000_000_001_000)
        self.assertEqual(L.ntp_ts_to_unix_ms(frac(0.9994)), 1_000_000_000_999)

    def test_eras(self):
        # a timestamp with the top bit set is 1968..2036; one with it clear is 2036..2104 (RFC 4330 section 3)
        self.assertEqual(L.ntp_ts_to_unix_ms(struct.pack(">II", 0xEDBD5F43, 0)), (0xEDBD5F43 - NTP_DELTA) * 1000)
        end_of_era0 = 2**32 - NTP_DELTA                                 # 2036-02-07 06:28:16 UTC
        self.assertEqual(datetime.datetime.fromtimestamp(end_of_era0, datetime.timezone.utc).isoformat(), "2036-02-07T06:28:16+00:00")
        self.assertEqual(L.ntp_ts_to_unix_ms(struct.pack(">II", 0xFFFFFFFF, 0)), (end_of_era0 - 1) * 1000)
        self.assertEqual(L.ntp_ts_to_unix_ms(struct.pack(">II", 0, 0)), end_of_era0 * 1000)
        self.assertEqual(L.ntp_ts_to_unix_ms(struct.pack(">II", 1, 0x80000000)), end_of_era0 * 1000 + 1500)
        self.assertEqual(self.conv(end_of_era0 * 1000), struct.pack(">II", 0, 0))      # (and the way back, with the era dropped)
        self.assertEqual(L.ntp_ts_to_unix_ms(struct.pack(">II", 0x80000000, 0)), (0x80000000 - NTP_DELTA) * 1000)   # 1968-01-20


class Request(unittest.TestCase):
    def test_layout(self):
        pkt = ctypes.create_string_buffer(48)
        L.ntp_make_request(pkt, T0 + 250, 0xBEEF)
        raw = pkt.raw
        self.assertEqual(raw[0], 0x23)                                   # leap 0, version 4, mode 3
        self.assertEqual(raw[1:40], bytes(39))                           # nothing but the transmit time is filled in
        want = bytearray(ts_bytes(T0 + 250))
        want[6:8] = b"\xbe\xef"                                          # the nonce replaces the low 16 bits of the fraction
        self.assertEqual(raw[40:48], bytes(want))

    def test_nonce_changes_only_the_low_bits(self):
        a, b = ctypes.create_string_buffer(48), ctypes.create_string_buffer(48)
        L.ntp_make_request(a, T0, 1)
        L.ntp_make_request(b, T0, 2)
        self.assertEqual(a.raw[:46], b.raw[:46])
        self.assertNotEqual(a.raw[46:], b.raw[46:])


class Replies(unittest.TestCase):
    def good(self, **kw):
        return reply(SENT, T0 + 100, T0 + 101, **kw)

    def test_accepts_a_good_reply(self):
        code, r = parse(self.good())
        self.assertEqual((code, r.stratum, r.version, r.li), (OK, 2, 4, 0))
        self.assertEqual((r.t2_ms, r.t3_ms), (T0 + 100, T0 + 101))
        self.assertEqual(r.root_ms, 0)

    def test_old_servers_answer_in_version_3(self):
        self.assertEqual(parse(self.good(vn=3))[0], OK)
        self.assertEqual(parse(self.good(vn=2))[0], E_VERSION)
        self.assertEqual(parse(self.good(vn=5))[0], E_VERSION)

    def test_must_be_a_server_reply(self):
        for mode in (0, 1, 2, 3, 5, 6, 7):
            self.assertEqual(parse(self.good(mode=mode))[0], E_MODE, mode)

    def test_short(self):
        pkt = self.good()
        for n in (0, 1, 47):
            r = Reply()
            self.assertEqual(L.ntp_parse_reply(pkt[:n], n, SENT, ctypes.byref(r)), E_SHORT)
        self.assertEqual(parse(pkt + b"\x00" * 20)[0], OK)               # (extension fields and a MAC after the 48 bytes are fine)

    def test_a_reply_that_does_not_echo_our_timestamp(self):
        self.assertEqual(parse(reply(SENT, T0, T0, origin=bytes(8)))[0], E_ORIGIN)
        self.assertEqual(parse(reply(SENT, T0, T0, origin=ts_bytes(T0)))[0], E_ORIGIN)
        flipped = bytearray(SENT)
        flipped[7] ^= 1                                                  # the last bit of the nonce
        self.assertEqual(parse(reply(SENT, T0, T0, origin=bytes(flipped)))[0], E_ORIGIN)

    def test_stratum_zero_is_a_kiss_of_death(self):
        code, r = parse(self.good(stratum=0, li=3, refid=b"RATE"))
        self.assertEqual((code, r.kiss), (E_KISS, b"RATE"))
        code, r = parse(self.good(stratum=0, refid=b"DENY"))
        self.assertEqual((code, r.kiss), (E_KISS, b"DENY"))
        code, r = parse(self.good(stratum=0, refid=b"\x01\x02\x03\x04"))
        self.assertEqual((code, r.kiss), (E_KISS, b""))                  # (unprintable bytes are not a code)

    def test_stratum_16_and_up_is_unsynchronised(self):
        for s in (16, 17, 255):
            self.assertEqual(parse(self.good(stratum=s))[0], E_STRATUM, s)
        for s in (1, 2, 15):
            self.assertEqual(parse(self.good(stratum=s))[0], OK, s)

    def test_alarm_leap_indicator(self):
        self.assertEqual(parse(self.good(li=3))[0], E_ALARM)
        for li in (0, 1, 2):
            self.assertEqual(parse(self.good(li=li))[0], OK, li)           # (a leap second announcement is not a problem)

    def test_timestamps_that_cannot_be_true(self):
        self.assertEqual(parse(reply(SENT, None, T0))[0], E_TIMES)       # the server never filled in its receive time
        self.assertEqual(parse(reply(SENT, T0, None))[0], E_TIMES)
        self.assertEqual(parse(reply(SENT, T0 + 5, T0))[0], E_TIMES)     # it transmitted before it received
        self.assertEqual(parse(reply(SENT, T0, T0 + 3000))[0], OK)
        floor = 1_577_836_800_000
        self.assertEqual(parse(reply(SENT, floor, floor))[0], OK)
        self.assertEqual(parse(reply(SENT, floor - 2, floor - 1))[0], E_TIMES)       # a dead battery clock: 2019
        self.assertEqual(parse(reply(SENT, 86_400_000, 86_400_000))[0], E_TIMES)     # 1970

    def test_root_distance(self):
        def secs16(s):
            return int(s * 65536)
        self.assertEqual(parse(self.good(root_delay=secs16(0.040), root_disp=secs16(0.010)))[0], OK)
        code, r = parse(self.good(root_delay=secs16(0.040), root_disp=secs16(0.010)))
        self.assertAlmostEqual(r.root_ms, 20 + 10, delta=3)              # half the delay plus the dispersion (the 16.16 numbers are cut to milliseconds)
        self.assertEqual(parse(self.good(root_delay=secs16(6.5), root_disp=0))[0], E_ROOT)
        self.assertEqual(parse(self.good(root_delay=0, root_disp=secs16(3.5)))[0], E_ROOT)
        self.assertEqual(parse(self.good(root_delay=-secs16(1), root_disp=0))[0], OK)   # a negative root delay counts as zero

    def test_error_texts(self):
        for c in range(E_ROOT + 1):
            self.assertTrue(L.ntp_strerror(c))
        self.assertEqual(L.ntp_strerror(99), b"unknown")


class Offsets(unittest.TestCase):
    def check(self, t1, t2, t3, t4):
        off, dl = ctypes.c_int64(), ctypes.c_int64()
        L.ntp_offset_delay(t1, t2, t3, t4, ctypes.byref(off), ctypes.byref(dl))
        return off.value, dl.value

    def test_a_worked_example(self):
        # the client's clock is 4 s behind the server's, each way takes 3 s, the server thinks for 1 s. The client sends at its 10 000;
        # the server's clock reads 17 000 on arrival (10 000 + 3 000 + 4 000) and 18 000 when it answers; the answer arrives 3 s later,
        # which is 21 000 by the server's clock and 17 000 by the client's.
        off, dl = self.check(10_000, 17_000, 18_000, 17_000)
        self.assertEqual((off, dl), (4_000, 6_000))

    def test_symmetric_paths_give_exactly_the_offset(self):
        for true_offset in (-7200_000, -1500, 0, 3, 1_000_000, 86_400_000 * 400):
            for one_way in (0, 1, 10, 250, 1400):
                t1 = 1_700_000_000_000
                t2 = t1 + one_way + true_offset
                t3 = t2 + 7
                t4 = t3 - true_offset + one_way
                off, dl = self.check(t1, t2, t3, t4)
                self.assertEqual(off, true_offset, (true_offset, one_way))
                self.assertEqual(dl, 2 * one_way)

    def test_random(self):
        rnd = random.Random(5)
        for _ in range(500):
            t1, t2, t3, t4 = (rnd.randrange(10**12, 2 * 10**12) for _ in range(4))
            off, dl = self.check(t1, t2, t3, t4)
            s = (t2 - t1) + (t3 - t4)
            self.assertEqual(off, -((-s) // 2) if s < 0 else s // 2)       # (truncated toward zero)
            self.assertEqual(dl, (t4 - t1) - (t3 - t2))

    def test_limits(self):
        self.assertTrue(L.ntp_delay_ok(0))
        self.assertTrue(L.ntp_delay_ok(3000))
        self.assertFalse(L.ntp_delay_ok(3001))
        self.assertTrue(L.ntp_delay_ok(-10))                              # rounding
        self.assertFalse(L.ntp_delay_ok(-11))                             # the clock moved under the exchange
        self.assertTrue(L.ntp_agree(5000, 6000))
        self.assertTrue(L.ntp_agree(6000, 5000))
        self.assertFalse(L.ntp_agree(5000, 6001))
        self.assertFalse(L.ntp_agree(7_200_000, 12))
        self.assertTrue(L.ntp_agree(-1000, 0))


class Lists(unittest.TestCase):
    def specs(self, text):
        p = ctypes.c_char_p(text.encode())
        out = []
        s = Spec()
        while L.ntp_next_spec(ctypes.byref(p), ctypes.byref(s)):
            out.append((s.host.decode(), s.port))
        return out

    def test_lists(self):
        self.assertEqual(self.specs("10.0.2.2"), [("10.0.2.2", 123)])
        self.assertEqual(self.specs("10.0.2.2:12345"), [("10.0.2.2", 12345)])
        self.assertEqual(self.specs("a.example,b.example:99"), [("a.example", 123), ("b.example", 99)])
        self.assertEqual(self.specs(",, a ,b,"), [("a", 123), ("b", 123)])   # (a space or an empty item is skipped)
        self.assertEqual(self.specs(""), [])

    def test_entries_that_make_no_sense_are_skipped(self):
        self.assertEqual(self.specs("good:1,bad:0,bad2:65536,bad3:x,bad4:,:80,ok"), [("good", 1), ("ok", 123)])
        self.assertEqual(self.specs("h:65535"), [("h", 65535)])
        self.assertEqual(self.specs("x" * 63), [("x" * 63, 123)])
        self.assertEqual(self.specs("x" * 64 + ",y"), [("y", 123)])      # too long for the field

    def test_ip4(self):
        ip = ctypes.c_uint32()
        for text, want in [("10.0.2.2", 0x0A000202), ("0.0.0.0", 0), ("255.255.255.255", 0xFFFFFFFF), ("192.168.1.1", 0xC0A80101)]:
            self.assertTrue(L.ntp_parse_ip4(text.encode(), ctypes.byref(ip)), text)
            self.assertEqual(ip.value, want)
        for text in ["", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.", "a.b.c.d", "1.2.3.4 ", " 1.2.3.4", "1..2.3", "1.2.3.04x", "pool.ntp.org", "1.2.3.1234"]:
            self.assertFalse(L.ntp_parse_ip4(text.encode(), ctypes.byref(ip)), text)


def dns_name(name):
    return b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split(".")) + b"\0"


def dns_answer(qid, name, answers, rcode=0, flags=0x8180, ra_pointer=True):
    """A response in the shape real resolvers send: the question echoed, answers that point back at it (compression)."""
    q = dns_name(name) + struct.pack(">HH", 1, 1)
    body = b""
    for kind, value in answers:
        owner = b"\xc0\x0c" if ra_pointer else dns_name(name)
        if kind == "A":
            body += owner + struct.pack(">HHIH", 1, 1, 60, 4) + bytes(int(x) for x in value.split("."))
        elif kind == "CNAME":
            rd = dns_name(value)
            body += owner + struct.pack(">HHIH", 5, 1, 60, len(rd)) + rd
        elif kind == "AAAA":
            body += owner + struct.pack(">HHIH", 28, 1, 60, 16) + bytes(16)
    return struct.pack(">HHHHHH", qid, flags | rcode, 1, len(answers), 0, 0) + q + body


class Dns(unittest.TestCase):
    def query(self, qid, name, cap=512):
        buf = ctypes.create_string_buffer(cap)
        n = L.ntp_dns_query(buf, cap, qid, name.encode())
        return buf.raw[:n]

    def answers(self, msg, qid, mx=4):
        ips = (ctypes.c_uint32 * mx)()
        n = L.ntp_dns_answers(msg, len(msg), qid, ips, mx)
        return n, [ips[i] for i in range(max(n, 0))]

    def test_query_bytes(self):
        want = struct.pack(">HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0) + dns_name("pool.ntp.org") + struct.pack(">HH", 1, 1)
        self.assertEqual(self.query(0x1234, "pool.ntp.org"), want)
        self.assertEqual(self.query(0x1234, "pool.ntp.org."), want)           # (a trailing dot is the root)
        self.assertEqual(self.query(7, "a"), struct.pack(">HHHHHH", 7, 0x0100, 1, 0, 0, 0) + b"\x01a\x00" + struct.pack(">HH", 1, 1))

    def test_names_that_cannot_be_asked(self):
        for name in ["", ".", "a..b", ".a", "a b", "a/b", "x" * 64 + ".org", "a." + "b" * 250 + ".c", "ünï.example"]:
            self.assertEqual(self.query(1, name), b"", name)
        self.assertNotEqual(self.query(1, "x" * 63 + ".org"), b"")
        self.assertNotEqual(self.query(1, "_ntp._udp.example-1.org"), b"")
        self.assertEqual(self.query(1, "pool.ntp.org", cap=20), b"")            # too small a buffer is refused, not overrun

    def test_answers(self):
        msg = dns_answer(0xBEEF, "pool.ntp.org", [("A", "10.1.2.3"), ("A", "10.1.2.4"), ("A", "192.0.2.77")])
        n, ips = self.answers(msg, 0xBEEF)
        self.assertEqual((n, ips), (3, [0x0A010203, 0x0A010204, 0xC000024D]))
        n, ips = self.answers(msg, 0xBEEF, mx=2)
        self.assertEqual((n, len(ips)), (2, 2))                                 # (never more than asked for)

    def test_cname_chains_and_other_record_types_are_stepped_over(self):
        msg = dns_answer(9, "time.example", [("CNAME", "real.example"), ("AAAA", ""), ("A", "203.0.113.5")])
        self.assertEqual(self.answers(msg, 9), (1, [0xCB007105]))
        msg = dns_answer(9, "time.example", [("CNAME", "real.example")])
        self.assertEqual(self.answers(msg, 9), (0, []))

    def test_owner_names_spelled_out(self):
        msg = dns_answer(3, "a.example", [("A", "10.9.8.7")], ra_pointer=False)
        self.assertEqual(self.answers(msg, 3), (1, [0x0A090807]))

    def test_not_ours_or_not_good(self):
        msg = dns_answer(5, "a.example", [("A", "192.168.1.1")])
        self.assertEqual(self.answers(msg, 6)[0], -1)                            # another transaction
        self.assertEqual(self.answers(msg[:11], 5)[0], -1)
        query_not_response = bytearray(msg)
        query_not_response[2] &= 0x7F
        self.assertEqual(self.answers(bytes(query_not_response), 5)[0], -1)
        for rcode in (2, 3, 5):
            self.assertEqual(self.answers(dns_answer(5, "nx.example", [], rcode=rcode), 5)[0], -2, rcode)
        self.assertEqual(self.answers(dns_answer(5, "a.example", []), 5), (0, []))

    def test_truncated_and_hostile_messages_do_not_crash(self):
        msg = dns_answer(5, "a.example", [("A", "192.168.1.1"), ("A", "192.168.1.2")])
        for cut in range(len(msg)):
            n, _ = self.answers(msg[:cut], 5)
            self.assertGreaterEqual(n, -2)
        rnd = random.Random(53)
        for _ in range(2000):
            junk = bytearray(msg)
            for _ in range(rnd.randrange(1, 6)):
                junk[rnd.randrange(len(junk))] = rnd.randrange(256)
            self.answers(bytes(junk), 5)
        loop = bytearray(struct.pack(">HHHHHH", 5, 0x8180, 1, 1, 0, 0) + b"\xc0\x0c" + struct.pack(">HH", 1, 1))        # a name that points at itself
        self.answers(bytes(loop), 5)
        self.assertEqual(self.answers(struct.pack(">HHHHHH", 5, 0x8180, 1, 0, 0, 0) + b"\x80", 5)[0], 0)             # a label type that does not exist


try:
    from zoneinfo import ZoneInfo
    ZoneInfo("America/New_York")
    HAVE_TZDATA = True
except Exception:                                    # no zoneinfo, or no tz database on this machine
    HAVE_TZDATA = False


class Zones(unittest.TestCase):
    def zone(self, text):
        z = Zone()
        ok = L.ntp_zone_parse(text.encode(), ctypes.byref(z))
        return ok, z

    def offset(self, z, t):
        return L.ntp_zone_offset(ctypes.byref(z), t)

    def test_fixed_zones(self):
        ok, z = self.zone("JST-9")
        self.assertTrue(ok)
        self.assertEqual((z.std_off, z.has_dst), (9 * 3600, False))
        self.assertEqual(self.offset(z, 0), 9 * 3600)
        ok, z = self.zone("UTC")
        self.assertFalse(ok)                                                   # no offset after the name: not a zone ... which means UTC
        self.assertEqual(z.std_off, 0)
        ok, z = self.zone("IST-5:30")
        self.assertEqual((ok, z.std_off), (True, 5 * 3600 + 1800))
        ok, z = self.zone("<-03>3")
        self.assertEqual((ok, z.std_off), (True, -3 * 3600))
        ok, z = self.zone("NST3:30NDT,M3.2.0,M11.1.0")
        self.assertEqual((ok, z.std_off, z.dst_off), (True, -(3 * 3600 + 1800), -(2 * 3600 + 1800)))

    def test_malformed_zones_are_utc(self):
        for text in ["", "nonsense", "EST", "EST5EDT,M3.2.0", "EST5EDT,J60,J300", "EST5EDT,M13.1.0,M11.1.0", "EST5EDT,M3.2.0,M11.1.0,x", "EST5,M3.2.0,M11.1.0", "ES5", "5EST"]:
            ok, z = self.zone(text)
            self.assertFalse(ok, text)
            self.assertEqual((z.std_off, z.dst_off, z.has_dst), (0, 0, False), text)
            self.assertEqual(self.offset(z, 1_793_000_000), 0)

    def test_the_hours_of_the_documented_transitions(self):
        # the exact seconds tests/case_time.py checks through the C library of the guest
        ok, z = self.zone("EST5EDT,M3.2.0,M11.1.0")
        self.assertTrue(ok)
        for t, want in [(1772953199, -5 * 3600), (1772953200, -4 * 3600), (1793512799, -4 * 3600), (1793512800, -5 * 3600),
                        (1782907200, -4 * 3600), (1796126400, -5 * 3600)]:
            self.assertEqual(self.offset(z, t), want, t)
        ok, z = self.zone("AEST-10AEDT,M10.1.0,M4.1.0/3")                      # the southern summer wraps around the new year
        self.assertEqual(self.offset(z, 1768435200), 11 * 3600)
        self.assertEqual(self.offset(z, 1784073600), 10 * 3600)
        ok, z = self.zone("EST5EDT")                                            # no rule: the United States'
        self.assertTrue(ok)
        self.assertEqual(self.offset(z, 1782907200), -4 * 3600)
        self.assertEqual(self.offset(z, 1796126400), -5 * 3600)

    @unittest.skipUnless(HAVE_TZDATA, "no time zone database on this machine")
    def test_against_the_tz_database(self):
        cases = [("EST5EDT,M3.2.0,M11.1.0", "America/New_York"), ("CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Paris"),
                 ("AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney"), ("JST-9", "Asia/Tokyo"), ("GMT0BST,M3.5.0/1,M10.5.0", "Europe/London"),
                 ("PST8PDT,M3.2.0,M11.1.0", "America/Los_Angeles")]
        rnd = random.Random(2026)
        for posix, name in cases:
            ok, z = self.zone(posix)
            self.assertTrue(ok, posix)
            info = ZoneInfo(name)
            instants = [rnd.randrange(1_735_689_600, 2_050_000_000) for _ in range(1500)]
            # and the seconds on both sides of every transition between 2025 and 2036, which is where a rule goes wrong
            probe = datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc)
            prev = probe.astimezone(info).utcoffset()
            t = int(probe.timestamp())
            while t < 2_085_978_496:
                t += 3600 * 6
                cur = datetime.datetime.fromtimestamp(t, datetime.timezone.utc).astimezone(info).utcoffset()
                if cur != prev:
                    lo, hi = t - 3600 * 6, t
                    while hi - lo > 1:
                        mid = (lo + hi) // 2
                        if datetime.datetime.fromtimestamp(mid, datetime.timezone.utc).astimezone(info).utcoffset() == prev:
                            lo = mid
                        else:
                            hi = mid
                    instants += [hi - 1, hi, hi + 1]
                    prev = cur
            for t in instants:
                want = int(datetime.datetime.fromtimestamp(t, datetime.timezone.utc).astimezone(info).utcoffset().total_seconds())
                self.assertEqual(self.offset(z, t), want, (posix, t))


if __name__ == "__main__":
    unittest.main()
