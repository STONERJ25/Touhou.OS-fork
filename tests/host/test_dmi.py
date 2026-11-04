#!/usr/bin/env python3
"""Host tests for the SMBIOS walker and the /proc/dmi text (kernel/arch/x86_64/dmi_parse.c), the part of the hardware report that
reads what the firmware says about the machine. The C file is built as a shared library with gcc (address and undefined-behaviour
sanitizers on, -Werror) and driven through ctypes. The tables are made here, in Python, from the layout in the DMTF's SMBIOS
Reference Specification (DSP0134), and every expectation is computed here from the same inputs, never from the C code.

    python tools/hosttest.py discover -s tests/host -p "test_dmi.py" -v
"""
import ctypes
import random
import shutil
import struct
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-dmi"


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    so = OUT / "dmi_parse.so"
    srcs = [REPO / "kernel" / "arch" / "x86_64" / "dmi_parse.c", REPO / "tests" / "host" / "dmi_helper.c"]
    deps = srcs + [REPO / "kernel" / "include" / "dmi.h", REPO / "kernel" / "include" / "types.h"]
    if so.exists() and all(d.stat().st_mtime <= so.stat().st_mtime for d in deps):
        return so
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
           "-I", str(REPO / "kernel" / "include"), *map(str, srcs), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of dmi_parse.c failed:\n" + r.stdout + r.stderr)
    return so


def build_fuzz():
    """The same walker linked into a program with the address sanitizer (a Python process cannot load that runtime late), or None without it."""
    exe = OUT / "dmi_fuzz"
    srcs = [REPO / "kernel" / "arch" / "x86_64" / "dmi_parse.c", REPO / "tests" / "host" / "dmi_fuzz.c"]
    deps = srcs + [REPO / "kernel" / "include" / "dmi.h"]
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return None
    r = subprocess.run([cc, "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(REPO / "kernel" / "include"), *map(str, srcs), "-o", str(exe)], capture_output=True, text=True)
    return exe if r.returncode == 0 else None


def lib():
    L = ctypes.CDLL(str(build()))
    L.dmi_info_size.restype = ctypes.c_size_t
    L.dmi_parse.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint16]
    L.dmi_format.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    L.dmi_format.restype = ctypes.c_size_t
    L.dmi_info_entry.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8]
    L.dmi_chassis_name.argtypes = [ctypes.c_uint8]
    L.dmi_chassis_name.restype = ctypes.c_char_p
    L.dmi_memory_type_name.argtypes = [ctypes.c_uint8]
    L.dmi_memory_type_name.restype = ctypes.c_char_p
    return L


# ---------------------------------------------------------------------------------------------------- building tables (DSP0134 section 6 and 7)

def structure(typ, formatted, strings=()):
    """header (type, length, handle) + formatted area + the string set (each string NUL terminated, the set ended by one more NUL)."""
    assert all(strings), "a string set holds no empty string (two NULs in a row end it): a missing string is index 0"
    body = bytes([typ, 4 + len(formatted)]) + struct.pack("<H", 0x100 + typ) + formatted
    strs = b"".join(s.encode("latin-1") + b"\0" for s in strings)
    return body + (strs if strs else b"\0") + b"\0"


def bios(vendor, version, date, major=0xFF, minor=0xFF, ec_major=0xFF, ec_minor=0xFF):
    f = bytes([1, 2]) + struct.pack("<H", 0xF000) + bytes([3, 0x40]) + bytes(8) + bytes(2) + bytes([major, minor, ec_major, ec_minor])
    return structure(0, f, [vendor, version, date])


UUID = bytes.fromhex("00112233445566778899aabbccddeeff")


def system(manufacturer, product, version, serial, uuid=UUID, sku="", family=""):
    strs = []

    def idx(s):
        if not s:
            return 0
        strs.append(s)
        return len(strs)
    m, p, v, sn, k, fam = idx(manufacturer), idx(product), idx(version), idx(serial), idx(sku), idx(family)
    return structure(1, bytes([m, p, v, sn]) + uuid + bytes([6, k, fam]), strs)


def board(manufacturer, product, version, serial, asset):
    f = bytes([1, 2, 3, 4, 5, 0x09, 0, 0, 0, 10, 0])
    return structure(2, f, [manufacturer, product, version, serial, asset])


def chassis(typ_byte, manufacturer, version, serial, asset=None):
    f = bytes([1, typ_byte, 2, 3, 4 if asset else 0]) + bytes([3, 3, 3, 3]) + bytes(4) + bytes([0, 1, 0, 0])
    return structure(3, f, [manufacturer, version, serial] + ([asset] if asset else []))


def processor(socket, manufacturer, version, max_mhz, cur_mhz, cores, enabled, threads, cores2=None, enabled2=None, threads2=None):
    f = bytearray(0x30 - 4)
    f[0] = 1                                     # 4: socket string
    f[1] = 3                                     # 5: central processor
    f[3] = 2                                     # 7: manufacturer string
    f[0x10 - 4] = 3                              # 0x10: version string
    struct.pack_into("<HHH", f, 0x12 - 4, 100, max_mhz, cur_mhz)
    f[0x23 - 4], f[0x24 - 4], f[0x25 - 4] = cores, enabled, threads
    struct.pack_into("<HHH", f, 0x2A - 4, cores2 or 0, enabled2 or 0, threads2 or 0)
    return structure(4, bytes(f), [socket, manufacturer, version])


def memory(locator, size_field, typ, speed=0, configured=0, manufacturer="", serial="", part="", ext=0):
    f = bytearray(0x22 - 4)
    strings = [locator, "BANK 0"]                # a string set holds no empty strings: a missing one is index 0
    idx = {}
    for name, s in (("manufacturer", manufacturer), ("serial", serial), ("part", part)):
        if s:
            strings.append(s)
            idx[name] = len(strings)
    struct.pack_into("<H", f, 0x0C - 4, size_field)
    f[0x0E - 4] = 9                              # DIMM
    f[0x10 - 4] = 1                              # locator
    f[0x11 - 4] = 2                              # bank locator
    f[0x12 - 4] = typ
    struct.pack_into("<H", f, 0x15 - 4, speed)
    f[0x17 - 4], f[0x18 - 4], f[0x1A - 4] = idx.get("manufacturer", 0), idx.get("serial", 0), idx.get("part", 0)
    struct.pack_into("<I", f, 0x1C - 4, ext)
    struct.pack_into("<H", f, 0x20 - 4, configured)
    return structure(17, bytes(f), strings)


END = structure(127, b"")


def parse_text(text):
    out = {}
    for line in text.splitlines():
        k, _, v = line.partition(": ")
        out[k] = v
    return out


class Dmi:
    """One DmiInfo buffer, filled by dmi_parse."""

    def __init__(self, table, ver=0x0208, kind=3, flags=0):
        self.L = lib()
        self.buf = ctypes.create_string_buffer(self.L.dmi_info_size())
        self.L.dmi_parse(self.buf, table, len(table), ver)
        self.L.dmi_info_entry(self.buf, kind, flags)

    def text(self, cap=16384):
        out = ctypes.create_string_buffer(cap)
        n = self.L.dmi_format(self.buf, out, cap)
        self.n = n
        return out.value.decode("latin-1")

    def keys(self):
        return parse_text(self.text())


def full_table(ver=0x0208):
    return b"".join([
        bios("Test BIOS Corp", "  v1.2.3  ", "11/03/2026", major=1, minor=2),
        system("Reimu Works", "Shrine Box 9", "rev A", "SN-0042", sku="SKU-9", family="Shrine"),
        board("Hakurei Parts", "HB-1", "v2", "BSN-77", "ASSET-1"),
        chassis(0x83, "Reimu Works", "c1", "CSN-1"),                    # bit 7 is the chassis lock: the type is 3, a desktop
        processor("SOCKET 0", "Test CPU Inc.", "Ultra Core 9", 4200, 3400, 4, 4, 8),
        processor("SOCKET 1", "Test CPU Inc.", "Ultra Core 9", 4200, 3600, 0xFF, 0xFF, 0xFF, cores2=300, enabled2=290, threads2=600),
        memory("DIMM A", 8192, 0x1A, speed=2400, configured=2133, manufacturer="Mem Co", serial="MSN-1", part="PART-1"),
        memory("DIMM B", 0, 0x02),
        memory("DIMM C", 0x8000 | 2048, 0x18),
        memory("DIMM D", 0x7FFF, 0x22, speed=4800, ext=65536),
        END,
    ])


class TestDmi(unittest.TestCase):
    def test_every_field(self):
        k = Dmi(full_table(), ver=0x0208, kind=3).keys()
        self.assertEqual(k["smbios"], "2.8 (64-bit entry point), 11 structures, %d bytes" % len(full_table()))
        self.assertEqual(k["bios.vendor"], "Test BIOS Corp")
        self.assertEqual(k["bios.version"], "v1.2.3")                    # padding trimmed
        self.assertEqual(k["bios.date"], "11/03/2026")
        self.assertEqual(k["bios.release"], "1.2")
        self.assertNotIn("bios.ec_release", k)                           # 0xFF: not stated
        self.assertEqual(k["system.manufacturer"], "Reimu Works")
        self.assertEqual(k["system.product"], "Shrine Box 9")
        self.assertEqual(k["system.version"], "rev A")
        self.assertEqual(k["system.serial"], "SN-0042")
        self.assertEqual(k["system.sku"], "SKU-9")
        self.assertEqual(k["system.family"], "Shrine")
        self.assertEqual(k["baseboard.product"], "HB-1")
        self.assertEqual(k["baseboard.serial"], "BSN-77")
        self.assertEqual(k["baseboard.asset"], "ASSET-1")
        self.assertEqual(k["chassis.type"], "3 (Desktop)")
        self.assertEqual(k["chassis.serial"], "CSN-1")
        self.assertNotIn("chassis.asset", k)
        self.assertEqual(k["processor.count"], "2")
        self.assertEqual(k["processor.0.socket"], "SOCKET 0")
        self.assertEqual(k["processor.0.version"], "Ultra Core 9")
        self.assertEqual((k["processor.0.max_mhz"], k["processor.0.current_mhz"]), ("4200", "3400"))
        self.assertEqual((k["processor.0.cores"], k["processor.0.threads"]), ("4", "8"))
        self.assertEqual((k["processor.1.cores"], k["processor.1.threads"]), ("300", "600"))      # 0xFF: the 16-bit fields of 3.0
        self.assertEqual(k["memory.slots"], "4 (3 filled)")
        self.assertEqual((k["memory.0.size_mib"], k["memory.0.type"], k["memory.0.speed_mts"], k["memory.0.configured_mts"]), ("8192", "DDR4", "2400", "2133"))
        self.assertEqual((k["memory.0.manufacturer"], k["memory.0.serial"], k["memory.0.part"]), ("Mem Co", "MSN-1", "PART-1"))
        self.assertEqual(k["memory.1.size_mib"], "0")                    # an empty slot says nothing else
        self.assertNotIn("memory.1.type", k)
        self.assertEqual((k["memory.2.size_mib"], k["memory.2.type"]), ("2", "DDR3"))     # the field was in KiB: 2048 KiB = 2 MiB
        self.assertEqual((k["memory.3.size_mib"], k["memory.3.type"]), ("65536", "DDR5"))  # 0x7FFF: the extended size

    def test_uuid_layout_depends_on_the_version(self):
        new = Dmi(full_table(), ver=0x0208).keys()["system.uuid"]
        old = Dmi(full_table(), ver=0x0203).keys()["system.uuid"]
        self.assertEqual(new, "33221100-5544-7766-8899-aabbccddeeff")    # 2.6 and later: the first three fields are little-endian
        self.assertEqual(old, "00112233-4455-6677-8899-aabbccddeeff")
        self.assertEqual(Dmi(full_table(), ver=0x0206).keys()["system.uuid"], new)
        self.assertEqual(Dmi(full_table(), ver=0x0300).keys()["system.uuid"], new)

    def test_uuid_all_zero_or_all_ones_is_absent(self):
        for u in (bytes(16), b"\xff" * 16):
            t = system("M", "P", "V", "S", uuid=u) + END
            self.assertNotIn("system.uuid", Dmi(t).keys())

    def test_missing_table(self):
        L = lib()
        buf = ctypes.create_string_buffer(L.dmi_info_size())
        L.dmi_parse(buf, None, 0, 0)
        out = ctypes.create_string_buffer(256)
        L.dmi_format(buf, out, 256)
        self.assertEqual(out.value.decode(), "smbios: not provided\n")

    def test_entry_point_notes(self):
        t = full_table()
        self.assertIn("entry point checksum bad", Dmi(t, flags=1).text().splitlines()[0])
        self.assertIn("32-bit entry point", Dmi(t, kind=2).text().splitlines()[0])
        self.assertNotIn("table cut short", Dmi(t).text())

    def test_strings_are_cleaned(self):
        t = system("AéB", "  padded  ", "", "S") + END                     # a non-ASCII byte becomes '?', padding goes
        k = Dmi(t).keys()
        self.assertEqual(k["system.manufacturer"], "A?B")
        self.assertEqual(k["system.product"], "padded")
        self.assertNotIn("system.version", k)

    def test_string_index_past_the_set_is_empty(self):
        f = bytes([1, 2, 9, 4]) + UUID + bytes([6, 0, 0])                 # product string 2 exists, version is string 9: not there
        t = structure(1, f, ["only", "two"]) + END
        k = Dmi(t).keys()
        self.assertEqual(k["system.manufacturer"], "only")
        self.assertNotIn("system.version", k)

    def test_short_structures_give_less_not_a_fault(self):
        # a BIOS structure of the old 2.0 length (no release fields), a memory device of 2.1 length (no speed), a processor of 2.0 length (no core counts)
        old_bios = structure(0, bytes([1, 2]) + struct.pack("<H", 0xF000) + bytes([3, 0x40]) + bytes(8), ["V", "1", "d"])
        old_mem = bytearray(0x15 - 4)
        struct.pack_into("<H", old_mem, 0x0C - 4, 1024)
        old_mem[0x10 - 4] = 1
        old_cpu = bytearray(0x1A - 4)
        old_cpu[0] = 1
        t = old_bios + structure(17, bytes(old_mem), ["SLOT"]) + structure(4, bytes(old_cpu), ["SK"]) + END
        k = Dmi(t).keys()
        self.assertEqual(k["bios.vendor"], "V")
        self.assertNotIn("bios.release", k)
        self.assertEqual(k["memory.0.size_mib"], "1024")
        self.assertNotIn("memory.0.speed_mts", k)
        self.assertEqual(k["processor.0.socket"], "SK")
        self.assertNotIn("processor.0.cores", k)

    def test_only_the_first_enclosure_is_the_machine(self):
        t = chassis(0x0A, "First", "v", "s1") + chassis(0x11, "Second", "v", "s2") + END
        k = Dmi(t).keys()
        self.assertEqual(k["chassis.type"], "10 (Notebook)")
        self.assertEqual(k["chassis.manufacturer"], "First")

    def test_table_ending_without_the_terminator_is_flagged(self):
        t = bios("V", "1", "d")                                         # no type 127
        d = Dmi(t)
        self.assertTrue(d.text().startswith("smbios:"))
        self.assertIn("table cut short", d.text())
        self.assertEqual(d.keys()["bios.vendor"], "V")

    def test_names(self):
        L = lib()
        self.assertEqual(L.dmi_chassis_name(3), b"Desktop")
        self.assertEqual(L.dmi_chassis_name(9), b"Laptop")
        self.assertEqual(L.dmi_chassis_name(0x24), b"Stick PC")
        self.assertIsNone(L.dmi_chassis_name(0x25))
        self.assertIsNone(L.dmi_chassis_name(0))
        self.assertEqual(L.dmi_memory_type_name(0x1A), b"DDR4")
        self.assertEqual(L.dmi_memory_type_name(0x22), b"DDR5")
        self.assertIsNone(L.dmi_memory_type_name(0xEE))

    def test_output_never_exceeds_the_buffer(self):
        t = full_table()
        for cap in (1, 2, 5, 17, 100, 333, 1000):
            d = Dmi(t)
            raw = ctypes.create_string_buffer(cap + 64)
            ctypes.memset(raw, 0x55, cap + 64)
            n = d.L.dmi_format(d.buf, raw, cap)
            self.assertLess(n, cap)
            self.assertEqual(raw.raw[n], 0)                                 # terminated
            self.assertEqual(raw.raw[cap:], b"\x55" * 64, "wrote past the buffer for cap %d" % cap)
        d = Dmi(t)
        self.assertEqual(d.L.dmi_format(d.buf, None, 0), 0)

    def test_every_truncation_is_survived(self):
        t = full_table()
        for n in range(len(t) + 1):
            d = Dmi(t[:n])
            d.text()                                                        # must not fault (the undefined-behaviour sanitizer is on)

    def damaged_tables(self, count):
        rng = random.Random(0xD31)
        t = bytearray(full_table())
        for _ in range(count):
            u = bytearray(t)
            for _ in range(rng.randint(1, 8)):
                u[rng.randrange(len(u))] = rng.randrange(256)
            if rng.random() < 0.3:
                del u[rng.randrange(len(u)):]
            yield rng.choice([0x0203, 0x0208, 0x0307]), bytes(u)

    def test_random_damage_is_survived(self):
        for ver, u in self.damaged_tables(3000):
            Dmi(u, ver=ver).text()

    def test_no_read_past_the_table_under_the_address_sanitizer(self):
        """Every truncation and a few thousand damaged tables, each in a heap block of exactly its length, through a program built with ASan."""
        exe = build_fuzz()
        if not exe:
            self.skipTest("this compiler has no address sanitizer")
        t = full_table()
        feed = bytearray()
        count = 0
        for n in range(len(t) + 1):
            feed += struct.pack("<II", 0x0208, n) + t[:n]
            count += 1
        for ver, u in self.damaged_tables(4000):
            feed += struct.pack("<II", ver, len(u)) + u
            count += 1
        r = subprocess.run([str(exe)], input=bytes(feed), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace")[-1500:])
        self.assertIn(b"%d cases" % count, r.stdout)

    def test_a_lying_header_length_cannot_run_off_the_table(self):
        t = bytearray(bios("V", "1", "d") + END)
        t[1] = 0xFF                                                         # the formatted area claims 255 bytes
        d = Dmi(bytes(t))
        self.assertIn("table cut short", d.text())

    def test_many_structures_are_counted_but_only_the_first_listed(self):
        t = b"".join(memory(f"D{i}", 1024, 0x1A) for i in range(40)) + b"".join(processor(f"S{i}", "m", "v", 1, 1, 1, 1, 1) for i in range(12)) + END
        k = Dmi(t).keys()
        self.assertEqual(k["memory.slots"], "40 (32 filled)")               # the first 32 are described, the number seen is 40
        self.assertEqual(k["processor.count"], "12")
        self.assertIn("processor.7.socket", k)
        self.assertNotIn("processor.8.socket", k)


if __name__ == "__main__":
    unittest.main()
