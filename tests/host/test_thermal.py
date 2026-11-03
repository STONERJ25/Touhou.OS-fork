#!/usr/bin/env python3
"""Host tests for the CPU sensor code that needs no machine: the decoding of the thermal and power registers, the units, the test hook's
table (kernel/arch/x86_64/thermal_core.c), and the Super I/O probe (kernel/arch/x86_64/superio.c) run against a model of the chips that records
every byte the probe writes to a port. The C files are built as shared libraries with gcc (UBSan on, -Werror) and driven through ctypes; the
expected numbers are worked out here from the Intel SDM's register layouts, not from the C code.

    python tools/hosttest.py discover -s tests/host -p "test_thermal.py" -v
"""
import ctypes
import random
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-thermal"
ARCH = REPO / "kernel" / "arch" / "x86_64"
INC = REPO / "kernel" / "include"


def build(name):
    OUT.mkdir(parents=True, exist_ok=True)
    so = OUT / f"{name}.so"
    src = ARCH / f"{name}.c"
    deps = [src] + sorted(INC.glob("*.h"))
    if so.exists() and all(d.stat().st_mtime <= so.stat().st_mtime for d in deps):
        return so
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(INC), str(src), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"build of {name}.c failed:\n" + r.stdout + r.stderr)
    return so


class Status(ctypes.Structure):
    _fields_ = [("valid", ctypes.c_bool), ("readout", ctypes.c_int), ("resolution", ctypes.c_int), ("status", ctypes.c_uint32), ("log", ctypes.c_uint32)]


class FakeEntry(ctypes.Structure):
    _fields_ = [("msr", ctypes.c_uint32), ("val", ctypes.c_uint64), ("step", ctypes.c_uint64), ("reads", ctypes.c_uint64)]


class Fake(ctypes.Structure):
    _fields_ = [("e", FakeEntry * 16), ("n", ctypes.c_int)]


TH = None


def setUpModule():
    global TH
    L = ctypes.CDLL(str(build("thermal_core")))
    L.thermal_decode_status.argtypes = [ctypes.c_uint64, ctypes.POINTER(Status)]
    L.thermal_decode_tjmax.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_int)]
    L.thermal_decode_tjmax.restype = ctypes.c_int
    L.thermal_flag_names.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
    L.thermal_flag_names.restype = ctypes.c_size_t
    L.thermal_bus_khz.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
    L.thermal_bus_khz.restype = ctypes.c_int
    L.thermal_rapl_units.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_int)]
    L.thermal_rapl_units.restype = ctypes.c_bool
    L.thermal_energy_uj.argtypes = [ctypes.c_uint32, ctypes.c_int]
    L.thermal_energy_uj.restype = ctypes.c_uint64
    L.thermal_power_mw.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint64]
    L.thermal_power_mw.restype = ctypes.c_uint64
    L.thermal_fake_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(Fake)]
    L.thermal_fake_parse.restype = ctypes.c_bool
    L.thermal_fake_read.argtypes = [ctypes.POINTER(Fake), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)]
    L.thermal_fake_read.restype = ctypes.c_bool
    TH = L


def decode(msr):
    s = Status()
    TH.thermal_decode_status(msr, ctypes.byref(s))
    return s


NAMES = ["hot", "prochot", "critical", "threshold1", "threshold2", "power_limit", "current_limit", "cross_domain"]


class Decode(unittest.TestCase):
    def test_a_kaby_lake_idle_reading(self):
        # IA32_THERM_STATUS = 0x883B0000: bit 31 reading valid, bits 30:27 resolution 1, bits 22:16 digital readout 0x3B = 59 degrees below TjMax
        s = decode(0x883B0000)
        self.assertEqual((s.valid, s.readout, s.resolution, s.status, s.log), (True, 59, 1, 0, 0))

    def test_invalid_reading(self):
        s = decode(0x003B0000)                                                    # bit 31 clear
        self.assertEqual((s.valid, s.readout), (False, 59))

    def test_fields_against_bit_arithmetic(self):
        rnd = random.Random(19)
        for _ in range(2000):
            v = rnd.getrandbits(64)
            s = decode(v)
            self.assertEqual(s.valid, bool(v >> 31 & 1))
            self.assertEqual(s.readout, v >> 16 & 0x7F)
            self.assertEqual(s.resolution, v >> 27 & 0xF)
            want_status = sum(1 << k for k in range(8) if v >> (2 * k) & 1)       # SDM: bit 0 thermal status, 2 PROCHOT, 4 critical, 6 threshold 1, 8 threshold 2, 10 power, 12 current, 14 cross-domain
            want_log = sum(1 << k for k in range(8) if v >> (2 * k + 1) & 1)      # ... each followed by its sticky log bit
            self.assertEqual((s.status, s.log), (want_status, want_log), hex(v))

    def test_named_flags(self):
        buf = ctypes.create_string_buffer(120)
        TH.thermal_flag_names(0, buf, 120)
        self.assertEqual(buf.value, b"-")
        TH.thermal_flag_names(0b00100001, buf, 120)                               # bit 0 (hot) and bit 5 (power limitation)
        self.assertEqual(buf.value, b"hot power_limit")
        TH.thermal_flag_names(0xFF, buf, 120)
        self.assertEqual(buf.value.decode(), " ".join(NAMES))
        small = ctypes.create_string_buffer(8)                                    # never overruns, always terminated
        TH.thermal_flag_names(0xFF, small, 8)
        self.assertEqual(small.value, b"hot proc"[:7])
        # a real log: the SDM's bits for "PROCHOT# happened (log) and is not asserted now" = bit 3 of the register
        s = decode(0x88290008)
        self.assertEqual((s.status, s.log), (0, 0b10))

    def test_tjmax(self):
        off = ctypes.c_int()
        self.assertEqual(TH.thermal_decode_tjmax(0x00640000, ctypes.byref(off)), 100)       # an i5-7500's MSR_TEMPERATURE_TARGET
        self.assertEqual(off.value, 0)
        self.assertEqual(TH.thermal_decode_tjmax(0x03640000, ctypes.byref(off)), 100)       # bits 29:24: a programmed offset of 3
        self.assertEqual(off.value, 3)
        self.assertEqual(TH.thermal_decode_tjmax(0x00550000, None), 85)


class Units(unittest.TestCase):
    def test_bus_clock(self):
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x9E), 100000)             # Kaby Lake / Coffee Lake desktop
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x2A), 100000)             # Sandy Bridge
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x1E), 133333)             # Nehalem
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x25), 133333)             # Westmere
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x37), 0)                  # Bay Trail: an Atom, another clock
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 6, 0x0F), 0)                  # Core 2
        self.assertEqual(TH.thermal_bus_khz(b"GenuineIntel", 15, 0x9E), 0)                 # not family 6
        self.assertEqual(TH.thermal_bus_khz(b"AuthenticAMD", 6, 0x9E), 0)
        self.assertEqual(TH.thermal_bus_khz(b"KVMKVMKVM", 6, 0x9E), 0)                     # (a shorter vendor string must not be read past its end)
        self.assertEqual(TH.thermal_bus_khz(b"", 6, 0x9E), 0)

    def test_rapl_unit_register(self):
        esu = ctypes.c_int()
        self.assertTrue(TH.thermal_rapl_units(0x000A0E03, ctypes.byref(esu)))              # the reset value on client parts since Sandy Bridge: energy unit 1/2^14 J (61 uJ)
        self.assertEqual(esu.value, 14)
        self.assertTrue(TH.thermal_rapl_units(0x00000500, ctypes.byref(esu)))
        self.assertEqual(esu.value, 5)
        self.assertFalse(TH.thermal_rapl_units(0, ctypes.byref(esu)))
        self.assertFalse(TH.thermal_rapl_units(0x000A0003, ctypes.byref(esu)))             # energy field 0
        self.assertFalse(TH.thermal_rapl_units(0x1F00, ctypes.byref(esu)))                 # 31: not credible

    def test_energy_in_microjoules(self):
        for raw in (0, 1, 16384, 0x1000000, 0xFFFFFFFF):
            for esu in (5, 14, 16, 30):
                self.assertEqual(TH.thermal_energy_uj(raw, esu), raw * 1_000_000 >> esu, (raw, esu))
        self.assertEqual(TH.thermal_energy_uj(16384, 14), 1_000_000)                      # 16384 units of 1/16384 J is one joule

    def test_power_from_the_counter(self):
        # 65 W for ten seconds is 650 J = 10 649 600 units of 1/16384 J
        self.assertEqual(TH.thermal_power_mw(1000, 1000 + 10_649_600, 14, 10_000), 65_000)
        # the 32-bit counter wraps: 0xFFFFFF00 -> 0x00000100 is 512 units
        self.assertEqual(TH.thermal_power_mw(0xFFFFFF00, 0x100, 14, 1000), (512 * 1_000_000 >> 14) // 1000)
        self.assertEqual(TH.thermal_power_mw(5, 5, 14, 1000), 0)
        self.assertEqual(TH.thermal_power_mw(0, 100, 14, 0), 0)                           # no window, no division by zero
        rnd = random.Random(611)
        for _ in range(500):
            a, b, esu, ms = rnd.getrandbits(32), rnd.getrandbits(32), rnd.randrange(5, 20), rnd.randrange(1, 1_200_000)
            self.assertEqual(TH.thermal_power_mw(a, b, esu, ms), (((b - a) & 0xFFFFFFFF) * 1_000_000 >> esu) // ms)


class FakeTable(unittest.TestCase):
    def parse(self, text):
        t = Fake()
        ok = TH.thermal_fake_parse(text.encode(), ctypes.byref(t))
        return ok, t

    def test_a_table(self):
        ok, t = self.parse("1a2:640000,19c:883b0000,611:1000000+2000,606:a0e03")
        self.assertTrue(ok)
        self.assertEqual(t.n, 4)
        self.assertEqual([(t.e[i].msr, t.e[i].val, t.e[i].step) for i in range(4)],
                         [(0x1A2, 0x640000, 0), (0x19C, 0x883B0000, 0), (0x611, 0x1000000, 0x2000), (0x606, 0xA0E03, 0)])
        v = ctypes.c_uint64()
        self.assertTrue(TH.thermal_fake_read(ctypes.byref(t), 0x19C, ctypes.byref(v)))
        self.assertEqual(v.value, 0x883B0000)
        self.assertTrue(TH.thermal_fake_read(ctypes.byref(t), 0x19C, ctypes.byref(v)))
        self.assertEqual(v.value, 0x883B0000)                                            # no step: always the same
        for k in range(4):                                                               # a step: each read is one step further
            self.assertTrue(TH.thermal_fake_read(ctypes.byref(t), 0x611, ctypes.byref(v)))
            self.assertEqual(v.value, 0x1000000 + k * 0x2000)
        self.assertFalse(TH.thermal_fake_read(ctypes.byref(t), 0x1B1, ctypes.byref(v)))   # not in the table: as if the CPU raised #GP

    def test_uppercase_and_long_values(self):
        ok, t = self.parse("19C:FFFFFFFFFFFFFFFF")
        self.assertTrue(ok)
        self.assertEqual(t.e[0].val, 0xFFFFFFFFFFFFFFFF)

    def test_rejected(self):
        for text in ["", "zzz", "19c", "19c:", ":1", "19c:1;1a2:2", "19c:1+", "19c:1+x", "19c:12345678901234567", "100000000:1", "19c:1 1a2:2", ","] + [",".join(f"{i:x}:1" for i in range(17))]:
            ok, _ = self.parse(text)
            self.assertFalse(ok, text)
        ok, t = self.parse("19c:1,")                                                      # (one comma too many at the end is forgiven)
        self.assertEqual((ok, t.n), (True, 1))
        ok, t = self.parse(",".join(f"{i + 0x100:x}:1" for i in range(16)))
        self.assertTrue(ok)
        self.assertEqual(t.n, 16)


# ------------------------------------------------------------------------------------------------------------ the Super I/O probe

class SioResult(ctypes.Structure):
    _fields_ = [("port", ctypes.c_uint16), ("family", ctypes.c_int), ("id", ctypes.c_uint16), ("chip", ctypes.c_char * 24), ("hwm_known", ctypes.c_bool),
                ("hwm_ldn", ctypes.c_int), ("hwm_active", ctypes.c_bool), ("hwm_base", ctypes.c_uint16)]


OUTF = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint8)
INF = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_uint16)


class Ports(ctypes.Structure):
    _fields_ = [("out", OUTF), ("in_", INF), ("ctx", ctypes.c_void_p)]


SIO_NONE, SIO_NUVOTON, SIO_ITE, SIO_SMSC = range(4)


class Chip:
    """A configuration port pair at `base` (0x2E or 0x4E). `kind`: nuvoton, ite, smsc (unlocked by their own key), ec (a device with fixed registers
    that ignores every key), quiet (nothing there: reads give 0xFF). Logs every access of the whole bus."""

    def __init__(self, kind, base, ident=0, ldn_regs=None):
        self.kind, self.base, self.ident = kind, base, ident
        self.ldn_regs = ldn_regs or {}                                # {ldn: {0x30: activate, 0x60: hi, 0x61: lo}}
        self.unlocked = False
        self.keys = []
        self.index = 0
        self.ldn = 0
        self.writes = []                                              # (port, value) of every write
        self.reads = []
        self.exited = 0

    def key_for(self):
        if self.kind == "nuvoton":
            return [0x87, 0x87]
        if self.kind == "ite":
            return [0x87, 0x01, 0x55, 0x55 if self.base == 0x2E else 0xAA]
        if self.kind == "smsc":
            return [0x55]
        return None

    def out(self, port, v):
        self.writes.append((port, v))
        if port not in (self.base, self.base + 1) or self.kind == "quiet":
            return
        if self.kind == "ec":                                        # index/data pair that behaves the same with or without a key: remembers the index only
            if port == self.base:
                self.index = v
            return
        if port == self.base:
            if not self.unlocked:
                want = self.key_for()
                self.keys.append(v)
                if self.keys == want[:len(self.keys)]:
                    if len(self.keys) == len(want):
                        self.unlocked = True
                        self.keys = []
                else:                                                # a wrong byte starts the sequence over (and may itself be the first byte of the key)
                    self.keys = [v] if v == want[0] else []
                return
            if self.kind in ("nuvoton", "smsc") and v == 0xAA:
                self.unlocked = False
                self.exited += 1
                return
            self.index = v
        elif self.unlocked:
            if self.index == 0x07:
                self.ldn = v
            elif self.kind == "ite" and self.index == 0x02 and v & 0x02:
                self.unlocked = False
                self.exited += 1

    def inp(self, port):
        self.reads.append(port)
        if port not in (self.base, self.base + 1) or self.kind == "quiet":
            return 0xFF
        if self.kind == "ec":
            return {0x20: 0x12, 0x21: 0x34}.get(self.index, 0x55) if port == self.base + 1 else 0x00
        if port == self.base:
            return self.index if self.unlocked else 0xFF
        if not self.unlocked:
            return 0xFF
        regs = {0x20: self.ident >> 8, 0x21: self.ident & 0xFF}
        regs.update(self.ldn_regs.get(self.ldn, {}))
        return regs.get(self.index, 0x00)


class Bus:
    def __init__(self, *chips):
        self.chips = chips
        self.log = []

    def out(self, port, v):
        self.log.append(("w", port, v))
        for c in self.chips:
            c.out(port, v)

    def inp(self, port):
        self.log.append(("r", port))
        for c in self.chips:
            if port in (c.base, c.base + 1):
                return c.inp(port)
        return 0xFF


SIO = None


def sio_lib():
    global SIO
    if SIO is None:
        L = ctypes.CDLL(str(build("superio")))
        L.superio_probe.argtypes = [ctypes.POINTER(Ports), ctypes.POINTER(SioResult * 2)]
        L.superio_probe.restype = ctypes.c_int
        L.superio_format.argtypes = [ctypes.POINTER(SioResult * 2), ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
        L.superio_format.restype = ctypes.c_size_t
        L.superio_family_name.argtypes = [ctypes.c_int]
        L.superio_family_name.restype = ctypes.c_char_p
        L.superio_lookup.argtypes = [ctypes.c_int, ctypes.c_uint16, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int)]
        L.superio_lookup.restype = ctypes.c_bool
        SIO = L
    return SIO


def probe(*chips):
    L = sio_lib()
    bus = Bus(*chips)
    out_cb = OUTF(lambda ctx, port, v: bus.out(port, v))
    in_cb = INF(lambda ctx, port: bus.inp(port))
    ports = Ports(out_cb, in_cb, None)
    res = (SioResult * 2)()
    n = L.superio_probe(ctypes.byref(ports), ctypes.byref(res))
    return n, res, bus


class SuperIo(unittest.TestCase):
    ITE_LDN = {0x04: {0x30: 0x01, 0x60: 0x02, 0x61: 0x90}}
    NUV_LDN = {0x0B: {0x30: 0x01, 0x60: 0x0A, 0x61: 0x30}}

    def data_writes(self, bus):
        """Every write to a data port (the port after an index port) and what the index port held before it."""
        out, last_index = [], {}
        for e in bus.log:
            if e[0] != "w":
                continue
            _, port, v = e
            if port in (0x2E, 0x4E):
                last_index[port] = v
            elif port in (0x2F, 0x4F):
                out.append((port, last_index.get(port - 1), v))
        return out

    def test_nothing_there(self):
        n, res, bus = probe()
        self.assertEqual(n, 0)
        self.assertEqual([r.family for r in res], [SIO_NONE, SIO_NONE])
        self.assertEqual(self.data_writes(bus), [])                                       # not one byte to a data port
        self.assertTrue(all(e[1] in (0x2E, 0x2F, 0x4E, 0x4F) for e in bus.log))           # and no other port was touched at all
        text = ctypes.create_string_buffer(2000)
        L = sio_lib()
        L.superio_format(ctypes.byref(res), n, text, 2000)
        self.assertEqual(text.value.decode().splitlines()[:3], ["superio: nothing answered", "port.0x2e: nothing answered", "port.0x4e: nothing answered"])

    def test_a_device_that_ignores_the_keys_is_not_a_super_io_chip(self):
        # an embedded controller with an index/data pair at 0x4E/0x4F whose registers read the same with or without a key
        ec = Chip("ec", 0x4E)
        n, res, bus = probe(ec)
        self.assertEqual((n, res[1].family), (0, SIO_NONE))
        self.assertEqual(self.data_writes(bus), [])                                       # in particular no ITE "exit" byte for it to swallow
        # it saw index writes only: the key bytes, and the register numbers that were read
        self.assertTrue(set(v for p, v in ec.writes if p == 0x4E) >= {0x87, 0x55, 0xAA, 0x20, 0x21})

    def test_nuvoton_nct6793d_at_2e(self):
        chip = Chip("nuvoton", 0x2E, 0xD123, self.NUV_LDN)                               # NCT6793D is 0xD120; the low nibble is the revision
        n, res, bus = probe(chip)
        r = res[0]
        self.assertEqual(n, 1)
        self.assertEqual((r.port, r.family, r.id, r.chip), (0x2E, SIO_NUVOTON, 0xD123, b"NCT6793D"))
        self.assertEqual((r.hwm_known, r.hwm_ldn, r.hwm_active, r.hwm_base), (True, 0x0B, True, 0x0A30))
        self.assertFalse(chip.unlocked, "configuration mode must be left again")
        self.assertEqual(self.data_writes(bus), [(0x2F, 0x07, 0x0B)])                     # the one data write: "select logical device 0x0B"
        self.assertEqual(res[1].family, SIO_NONE)

    def test_ite_it8728f_at_4e(self):
        chip = Chip("ite", 0x4E, 0x8728, self.ITE_LDN)
        n, res, bus = probe(chip)
        r = res[1]
        self.assertEqual(n, 1)
        self.assertEqual((r.port, r.family, r.id, r.chip), (0x4E, SIO_ITE, 0x8728, b"IT8728F"))
        self.assertEqual((r.hwm_known, r.hwm_ldn, r.hwm_active, r.hwm_base), (True, 0x04, True, 0x0290))
        self.assertFalse(chip.unlocked)
        # the key on port 0x4E ends in 0xAA (on 0x2E it ends in 0x55), and the lock is the ITE one: register 2, bit 1
        self.assertEqual(self.data_writes(bus), [(0x4F, 0x07, 0x04), (0x4F, 0x02, 0x02)])
        keys = [v for p, v in chip.writes if p == 0x4E]
        self.assertIn([0x87, 0x01, 0x55, 0xAA], [keys[i:i + 4] for i in range(len(keys) - 3)])

    def test_ite_key_on_2e(self):
        chip = Chip("ite", 0x2E, 0x8628, self.ITE_LDN)
        n, res, bus = probe(chip)
        self.assertEqual((n, res[0].chip, res[0].family), (1, b"IT8628E", SIO_ITE))
        keys = [v for p, v in chip.writes if p == 0x2E]
        self.assertIn([0x87, 0x01, 0x55, 0x55], [keys[i:i + 4] for i in range(len(keys) - 3)])

    def test_smsc_is_found_by_its_device_id(self):
        chip = Chip("smsc", 0x2E, 0x8901)                                                 # device id 0x89 (SCH5027), revision 1
        n, res, bus = probe(chip)
        self.assertEqual((n, res[0].family, res[0].id, res[0].chip), (1, SIO_SMSC, 0x8901, b"SCH5027"))
        self.assertFalse(res[0].hwm_known)                                                # no logical device is assumed for the monitor of this family
        self.assertEqual(self.data_writes(bus), [])
        self.assertFalse(chip.unlocked)

    def test_a_chip_the_table_does_not_know_is_reported_not_poked(self):
        chip = Chip("ite", 0x2E, 0x8999, self.ITE_LDN)
        n, res, bus = probe(chip)
        r = res[0]
        self.assertEqual((n, r.family, r.id, r.chip, r.hwm_known), (1, SIO_ITE, 0x8999, b"", False))
        self.assertEqual(self.data_writes(bus), [])                                       # nothing written to a data port for a chip we know nothing about: no logical device
                                                                                          # selected, and not even ITE's lock (it may be another family's register 2)
        self.assertTrue(chip.unlocked)                                                    # so it stays in configuration mode, which the docs say

    def test_an_smsc_chip_is_never_sent_the_ite_sequence(self):
        # ITE's key contains SMSC's whole key (0x55): had the ITE sequence been sent first, this chip would have been unlocked by it and handed ITE's lock
        chip = Chip("smsc", 0x2E, 0x8601)
        n, res, bus = probe(chip)
        self.assertEqual((n, res[0].chip), (1, b"SCH5127"))
        sent = [v for p, v in chip.writes if p == 0x2E]
        self.assertNotIn([0x87, 0x01, 0x55, 0x55], [sent[i:i + 4] for i in range(len(sent) - 3)])
        # an SMSC chip whose id is NOT in the table still is not handed the ITE lock, although its id looks like an ITE one (top nibble 8)
        chip = Chip("smsc", 0x2E, 0x8A01)
        n, res, bus = probe(chip)
        self.assertEqual((n, res[0].family, res[0].id), (1, SIO_SMSC, 0x8A01))
        self.assertEqual(self.data_writes(bus), [])

    def test_two_chips_one_each(self):
        a, b = Chip("nuvoton", 0x2E, 0xC562, self.NUV_LDN), Chip("ite", 0x4E, 0x8620, self.ITE_LDN)
        n, res, bus = probe(a, b)
        self.assertEqual((n, res[0].chip, res[1].chip), (2, b"NCT6779D", b"IT8620E"))
        self.assertFalse(a.unlocked or b.unlocked)

    def test_the_monitor_can_be_off(self):
        chip = Chip("nuvoton", 0x2E, 0xC803, {0x0B: {0x30: 0x00, 0x60: 0x00, 0x61: 0x00}})
        n, res, bus = probe(chip)
        r = res[0]
        self.assertEqual((r.chip, r.hwm_known, r.hwm_active, r.hwm_base), (b"NCT6791D", True, False, 0))

    def test_only_the_configuration_ports_are_ever_written(self):
        for chips in ([], [Chip("nuvoton", 0x2E, 0xD123, self.NUV_LDN)], [Chip("ite", 0x4E, 0x8728, self.ITE_LDN)], [Chip("ec", 0x2E)],
                      [Chip("smsc", 0x4E, 0x8601), Chip("ite", 0x2E, 0x8712, self.ITE_LDN)]):
            n, res, bus = probe(*chips)
            ports = {e[1] for e in bus.log}
            self.assertTrue(ports <= {0x2E, 0x2F, 0x4E, 0x4F}, ports)
            bad = [e for e in bus.log if e[0] == "w" and e[1] in (0x2F, 0x4F) and e[2] not in (0x00, 0x04, 0x0B, 0x02, 0x07)]
            self.assertEqual(bad, [])

    def test_the_table(self):
        L = sio_lib()
        chip, ldn = ctypes.c_char_p(), ctypes.c_int()
        cases = [(SIO_NUVOTON, 0xD428, b"NCT6798D", 0x0B), (SIO_NUVOTON, 0xD420, b"NCT6796D", 0x0B), (SIO_NUVOTON, 0xD120, b"NCT6793D", 0x0B), (SIO_NUVOTON, 0xD443, b"NCT6686D", -1),
                 (SIO_ITE, 0x8628, b"IT8628E", 0x04), (SIO_SMSC, 0x7C05, b"SCH3112", -1)]
        for fam, ident, name, want_ldn in cases:
            self.assertTrue(L.superio_lookup(fam, ident, ctypes.byref(chip), ctypes.byref(ldn)), hex(ident))
            self.assertEqual((chip.value, ldn.value), (name, want_ldn))
        for fam, ident in [(SIO_NUVOTON, 0xFFFF), (SIO_NUVOTON, 0x0000), (SIO_ITE, 0xD120), (SIO_NUVOTON, 0x8628), (SIO_ITE, 0x8999), (SIO_SMSC, 0x0100)]:
            self.assertFalse(L.superio_lookup(fam, ident, None, None), hex(ident))
        self.assertEqual(L.superio_family_name(SIO_ITE), b"ITE")

    def test_the_report(self):
        n, res, bus = probe(Chip("ite", 0x4E, 0x8728, self.ITE_LDN))
        text = ctypes.create_string_buffer(2000)
        L = sio_lib()
        L.superio_format(ctypes.byref(res), n, text, 2000)
        lines = text.value.decode().splitlines()
        self.assertEqual(lines[0], "superio: a chip answered")
        self.assertEqual(lines[1], "port.0x2e: nothing answered")
        self.assertIn("port.0x4e.family: ITE", lines)
        self.assertIn("port.0x4e.id: 0x8728", lines)
        self.assertIn("port.0x4e.chip: IT8728F", lines)
        self.assertIn("port.0x4e.hwm: logical device 0x04, active, io base 0x0290", lines)
        self.assertTrue(lines[-1].startswith("fans: not read"))
        tiny = ctypes.create_string_buffer(30)                                            # a small buffer is filled to the end, never past it
        got = L.superio_format(ctypes.byref(res), n, tiny, 30)
        self.assertLess(got, 30)
        self.assertEqual(tiny.value, text.value[:len(tiny.value)])


if __name__ == "__main__":
    unittest.main()
