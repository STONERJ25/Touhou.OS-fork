#!/usr/bin/env python3
"""Boots a PC-98 disk image in the headless harness tests/host/pc98run.c (the HLE BIOS of user/lib/pc98, docs/PC98_BIOS.md).

  python tools/pc98run.py --disk "assets/local/(TH01-05)/disks/th01.hdi" --cycles 400 --dump-text --png build/pc98/th01.png
  python tools/pc98run.py --disk IMAGE --trace-int=1b,18 --trace-work --until-key-wait --history 64 > build/pc98/trace.txt

Every option except --png, --dis and --rebuild goes to pc98run unchanged (its header lists them: --cpu, --cycles in millions, --keys, --trace-*, --font ...).
--dis SEG:OFF:LEN (hex) disassembles that part of the machine's memory when the run ends (with ndisasm from the nasm install; combine with --break SEG:OFF
or --until-exec to stop where you want to look).
--png FILE renders the final screen to a PNG (pc98run writes a PPM, this script converts it with zlib); --png-at N:FILE does the same when N million cycles
have run (repeatable). --wav, --opna-log and --opna-rhythm take paths like --disk. The harness is built with gcc -O2 into
build/host-x86-wsl (through WSL's Ubuntu on Windows, like tools/hosttest.py; natively elsewhere) whenever a source is newer than the binary.
Disk images are only read. The owner's images and font ROM stay where they are: never copy them into the tree.
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "user" / "lib" / "pc98"
SRCS = [ROOT / "tests" / "host" / "pc98run.c", ROOT / "shared" / "font8x16.c"] + sorted((LIB / "src").glob("*.c"))
DEPS = SRCS + sorted((LIB / "include").glob("*.h")) + [ROOT / "shared" / "font8x16.h"]
WINDOWS = os.name == "nt"
IN_WSL = not WINDOWS and "microsoft" in os.uname().release.lower()
OUT = ROOT / "build" / ("host-x86-wsl" if WINDOWS or IN_WSL else "host-x86")
EXE = OUT / "pc98run-fast"           # optimised, no sanitizers (the tests build their own "pc98run" with them)


def wsl_path(p):
    p = Path(p).resolve()
    return f"/mnt/{p.drive[:1].lower()}/" + str(p)[3:].replace("\\", "/")


def shell(cmd):
    """Runs an argv list on the build host (WSL's Ubuntu on Windows)."""
    if not WINDOWS:
        return subprocess.run(cmd)
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        sys.exit("pc98run: no wsl.exe: install WSL with a distribution that has gcc, or run on Linux")
    quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in cmd)
    return subprocess.run([wsl, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "sh", "-c", quoted])


def host_path(p):
    return wsl_path(p) if WINDOWS else str(p)


def build(force=False):
    if not force and EXE.exists() and all(d.stat().st_mtime <= EXE.stat().st_mtime for d in DEPS):
        return
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-O2", "-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-I", host_path(LIB / "include"), *[host_path(s) for s in SRCS], "-o", host_path(EXE)]
    if shell(cmd).returncode != 0:
        sys.exit("pc98run: build failed")


def write_png(ppm, png):
    data = Path(ppm).read_bytes()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        sys.exit("pc98run: %s is not a P6 PPM" % ppm)
    w, h = map(int, parts[1].split())
    pixels = parts[3]
    rows = b"".join(b"\x00" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    Path(png).parent.mkdir(parents=True, exist_ok=True)
    Path(png).write_bytes(out)


def find_ndisasm():
    exe = shutil.which("ndisasm")
    if exe:
        return exe
    cand = Path(os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM\ndisasm.exe"))
    return str(cand) if cand.is_file() else None


def disassemble(spec):
    """--dis SEG:OFF:LEN (hex): disassembles LEN bytes of the machine's memory at the end of the run (nasm's ndisasm, 16-bit with
    the 66h/67h prefixes decoded, so the 386 instructions of the games read right). Output goes to stdout."""
    seg, off, length = (int(x, 16) for x in spec.split(":"))
    exe = find_ndisasm()
    if not exe:
        print("pc98run: no ndisasm (comes with nasm)")
        return
    raw = ROOT / "build" / "pc98" / "dis.bin"
    data = raw.read_bytes() if raw.exists() else b""
    print("; %04X:%04X, %d bytes" % (seg, off, len(data)))
    subprocess.run([exe, "-b", "16", "-o", hex(off), str(raw)])


def main(argv):
    args, png, force, dis, pngs_at = [], None, False, None, []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--png" and i + 1 < len(argv):
            png = argv[i + 1]
            i += 2
            continue
        if a == "--png-at" and i + 1 < len(argv):       # N:FILE.png: a picture when N million cycles have run (repeatable)
            at, _, name = argv[i + 1].partition(":")
            ppm = ROOT / "build" / "pc98" / (Path(name).stem + ".ppm")
            ppm.parent.mkdir(parents=True, exist_ok=True)
            pngs_at.append((ppm, name))
            args += ["--ppm-at", "%s:%s" % (at, host_path(ppm))]
            i += 2
            continue
        if a == "--rebuild":
            force = True
            i += 1
            continue
        if a == "--dis" and i + 1 < len(argv):
            dis = argv[i + 1]
            seg, off, length = (int(x, 16) for x in dis.split(":"))
            raw = ROOT / "build" / "pc98" / "dis.bin"
            raw.parent.mkdir(parents=True, exist_ok=True)
            if raw.exists():
                raw.unlink()
            args += ["--dumpbin", "%X:%X:%s" % ((seg << 4) + off, length, host_path(raw))]
            i += 2
            continue
        if a in ("--disk", "--font", "--ppm", "--wav", "--opna-log", "--opna-rhythm") and i + 1 < len(argv):
            if a in ("--wav", "--opna-log"):
                Path(argv[i + 1]).resolve().parent.mkdir(parents=True, exist_ok=True)
            args += [a, host_path(argv[i + 1])]
            i += 2
            continue
        args.append(a)
        i += 1
    build(force)
    ppm = None
    if png:
        ppm = ROOT / "build" / "pc98" / (Path(png).stem + ".ppm")
        ppm.parent.mkdir(parents=True, exist_ok=True)
        args += ["--ppm", host_path(ppm)]
    sys.stdout.flush()
    rc = shell([host_path(EXE), *args]).returncode
    if png and ppm.exists():
        write_png(ppm, png)
        print("PNG %s" % png)
    for p, name in pngs_at:
        if p.exists():
            write_png(p, name)
            print("PNG %s" % name)
    if dis:
        disassemble(dis)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
