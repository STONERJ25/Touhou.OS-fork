#!/usr/bin/env python3
"""Host tests for winrun's PE32+ parser (user/bin/winrun/pe.c, docs/WINDOWS.md): the parser is built with gcc and the address and undefined-behaviour
sanitizers into tests/win/petool.c, run on the Windows test programs that tools/make-winprogs.py builds with the LLVM tools, and cross-checked against
llvm-readobj (headers, sections, imports, relocations). Then damaged copies: truncated, bit-flipped and overwritten files with fixed seeds (repeatable),
every one refused or loaded without a crash, and hand-made edge cases (an import table outside the image, a section over the headers, ...), each refused
with its own reason. The edge cases are made here from the layout in Microsoft's "PE Format" specification, not from the C code.

    python tools/hosttest.py discover -s tests/host -p "test_winrun.py" -v
"""
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-winrun"
PROGS = REPO / "build" / "winprogs"
NAMES = ["hello", "exitcode", "args", "heap", "unused_import", "called_import", "reloc", "reloc_low", "otherdll", "files"]

sys.path.insert(0, str(REPO / "tools"))


def llvm(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location("make_winprogs", REPO / "tools" / "make-winprogs.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.tool(name)


def build_programs():
    r = subprocess.run([sys.executable, str(REPO / "tools" / "make-winprogs.py")], capture_output=True, text=True)
    if r.returncode:
        raise unittest.SkipTest("cannot build the Windows test programs: " + r.stdout + r.stderr)


def build_tool():
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "petool"
    srcs = [REPO / "user" / "bin" / "winrun" / "pe.c", REPO / "tests" / "win" / "petool.c"]
    deps = srcs + [REPO / "user" / "bin" / "winrun" / "pe.h"]
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    r = subprocess.run([cc, "-O1", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(REPO / "user" / "bin" / "winrun"), *map(str, srcs), "-o", str(exe)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("build of petool failed:\n" + r.stdout + r.stderr)
    return exe


def petool(*args):
    r = subprocess.run([str(build_tool()), *map(str, args)], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, f"petool {args} crashed (status {r.returncode}):\n{r.stdout[-2000:]}\n{r.stderr[-4000:]}"
    return r.stdout


def readobj(path):
    """What llvm-readobj says about a file: (image base, entry, size of image, [sections], [(dll, name)], number of DIR64 relocations)."""
    rel = Path(path).relative_to(REPO).as_posix()
    r = subprocess.run([llvm("llvm-readobj"), "--file-headers", "--sections", "--coff-imports", "--coff-basereloc", rel], cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    t = r.stdout
    num = lambda k: int(re.search(rf"^\s*{k}: (\S+)", t, re.M).group(1), 0)
    secs = []
    for m in re.finditer(r"Name: (\S+) \(.*?\)\n\s*VirtualSize: (\S+)\n\s*VirtualAddress: (\S+)\n\s*RawDataSize: (\S+)\n\s*PointerToRawData: (\S+)\n(?:.*\n)*?\s*Characteristics \[ \((\S+)\)", t):
        secs.append((m.group(1), *(int(m.group(i), 0) for i in range(3, 1, -1)), int(m.group(5), 0), int(m.group(4), 0), int(m.group(6), 0)))
    imps = []
    for block in re.finditer(r"Import \{\n\s*Name: (\S+)\n(.*?)\n\s*\}", t, re.S):
        for s in re.finditer(r"Symbol: (\S+)", block.group(2)):
            imps.append((block.group(1), s.group(1)))
    relocs = len(re.findall(r"Type: DIR64", t))
    return num("ImageBase"), num("AddressOfEntryPoint"), num("SizeOfImage"), secs, imps, relocs


def parse_petool(text):
    base = entry = size = None
    secs, imps, relocs = [], [], None
    for line in text.splitlines():
        w = line.split()
        if w[0] == "base":
            base, entry, size = int(w[1], 16), int(w[3], 16), int(w[5], 16)
        elif w[0] == "section":
            secs.append((w[1], int(w[2], 16), int(w[3], 16), int(w[4], 16), int(w[5], 16), int(w[6], 16)))
        elif w[0] == "import":
            imps.append((w[1], w[2]))
        elif w[0] == "relocs":
            relocs = int(w[1])
        elif w[0] == "refused:":
            raise AssertionError("refused a valid program: " + line)
    return base, entry, size, secs, imps, relocs


class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_programs()
        build_tool()


class CrossCheck(Fixture):
    def test_every_program_matches_llvm_readobj(self):
        """Image base, entry point, SizeOfImage, every section (RVA, VirtualSize, raw offset and size, characteristics), every import and the number of
        DIR64 relocations: the parser's view and llvm-readobj's are the same"""
        for n in NAMES:
            with self.subTest(n):
                exe = PROGS / f"{n}.exe"
                mine = parse_petool(petool(exe))
                theirs = readobj(exe)
                self.assertEqual(mine[:3], theirs[:3])
                self.assertEqual(mine[3], theirs[3])
                self.assertEqual(sorted(mine[4]), sorted(theirs[4]))
                self.assertEqual(mine[5], theirs[5])
                self.assertTrue(mine[3] and mine[4], "a test program without sections or imports")

    def test_reloc_has_relocations(self):
        """The relocation test program has DIR64 relocations at both bases (the loader test depends on it)"""
        for n in ("reloc", "reloc_low"):
            self.assertGreaterEqual(parse_petool(petool(PROGS / f"{n}.exe"))[5], 5)


class Fuzz(Fixture):
    def test_damaged_files_never_crash(self):
        """3000 damaged copies of each of three programs, fixed seeds: refused or loaded, never a crash or a sanitizer report"""
        for n, seed in (("hello", 1), ("heap", 2), ("reloc", 3)):
            with self.subTest(n):
                out = petool("--fuzz", PROGS / f"{n}.exe", seed, 3000)
                m = re.match(r"accepted (\d+) refused (\d+)", out)
                self.assertTrue(m, out)
                self.assertGreater(int(m.group(2)), 1000, "most damaged files should be refused")


# ---------------------------------------------------------------------- hand-made damage, from the specification's layout

def headers(b):
    """Offsets of the parts of a PE32+ file (the specification: e_lfanew at 0x3C, COFF header after the signature, the optional header after it)."""
    lfanew = struct.unpack_from("<I", b, 0x3C)[0]
    coff = lfanew + 4
    nsec, optsize = struct.unpack_from("<H", b, coff + 2)[0], struct.unpack_from("<H", b, coff + 16)[0]
    opt = coff + 20
    return {"lfanew": lfanew, "coff": coff, "opt": opt, "sect": opt + optsize, "nsec": nsec, "dirs": opt + 112}


class EdgeCases(Fixture):
    def refused(self, b, why):
        p = OUT / "edge.exe"
        p.write_bytes(bytes(b))
        out = petool(p)
        last = out.splitlines()[-1] if out else ""
        self.assertTrue(last.startswith("refused: "), out)
        self.assertIn(why, last)

    def base(self, name="hello"):
        return bytearray((PROGS / f"{name}.exe").read_bytes())

    def test_not_mz(self):
        b = self.base(); b[0] = ord("X")
        self.refused(b, "no MZ header")

    def test_lfanew_outside(self):
        b = self.base(); struct.pack_into("<I", b, 0x3C, len(b) - 2)
        self.refused(b, "e_lfanew")

    def test_wrong_machine(self):
        b = self.base(); h = headers(b); struct.pack_into("<H", b, h["coff"], 0x14C)
        self.refused(b, "not x86-64")

    def test_pe32_magic(self):
        b = self.base(); h = headers(b); struct.pack_into("<H", b, h["opt"], 0x10B)
        self.refused(b, "not PE32+")

    def test_gui_subsystem(self):
        b = self.base(); h = headers(b); struct.pack_into("<H", b, h["opt"] + 68, 2)
        self.refused(b, "GUI")

    def test_import_table_outside_the_file(self):
        b = self.base(); h = headers(b); struct.pack_into("<I", b, h["dirs"] + 8, 0x7FFF0000)
        self.refused(b, "data directory 1")

    def test_import_name_runs_off_the_image(self):
        """The DLL name's RVA at the last byte of the image: no terminator inside it"""
        b = self.base(); h = headers(b)
        imp = struct.unpack_from("<I", b, h["dirs"] + 8)[0]
        size = struct.unpack_from("<I", b, h["opt"] + 56)[0]
        # the import directory's file offset: the section that holds its RVA
        for i in range(h["nsec"]):
            s = h["sect"] + 40 * i
            vs, va, rs, rp = struct.unpack_from("<IIII", b, s + 8)
            if va <= imp < va + max(vs, rs):
                struct.pack_into("<I", b, rp + imp - va + 12, size - 1)
        self.refused(b, "DLL name")

    def test_section_over_the_headers(self):
        b = self.base(); h = headers(b); struct.pack_into("<I", b, h["sect"] + 20, 0)       # first section's PointerToRawData = 0
        self.refused(b, "overlaps the headers")

    def test_overlapping_sections(self):
        b = self.base(); h = headers(b)
        va0 = struct.unpack_from("<I", b, h["sect"] + 12)[0]
        struct.pack_into("<I", b, h["sect"] + 40 + 12, va0)                                    # second section at the first one's RVA
        self.refused(b, "overlaps")

    def test_size_of_image_too_big_and_too_small(self):
        b = self.base(); h = headers(b); size = struct.unpack_from("<I", b, h["opt"] + 56)[0]
        struct.pack_into("<I", b, h["opt"] + 56, size + 0x1000)
        self.refused(b, "SizeOfImage")
        struct.pack_into("<I", b, h["opt"] + 56, size - 0x1000)
        self.refused(b, "past SizeOfImage")

    def test_truncated_section_table(self):
        b = self.base(); h = headers(b)
        self.refused(b[:h["sect"] + 20], "section table")

    def test_raw_data_past_the_end(self):
        b = self.base()
        self.refused(b[:len(b) - 512], "past the end of the file")

    def test_relocation_outside_the_image(self):
        b = self.base("reloc"); h = headers(b)
        rva = struct.unpack_from("<I", b, h["dirs"] + 40)[0]
        for i in range(h["nsec"]):
            s = h["sect"] + 40 * i
            vs, va, rs, rp = struct.unpack_from("<IIII", b, s + 8)
            if va <= rva < va + max(vs, rs):
                struct.pack_into("<I", b, rp + rva - va, 0x7FFF000)                               # the first block's page RVA
        self.refused(b, "outside the image")

    def test_entry_outside_code(self):
        b = self.base(); h = headers(b); struct.pack_into("<I", b, h["opt"] + 16, 0x10)
        self.refused(b, "entry point")


if __name__ == "__main__":
    unittest.main()
