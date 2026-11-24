#!/usr/bin/env python3
"""Builds the Windows test programs of winrun (docs/WINDOWS.md) with the LLVM tools: tests/win/src/*.c -> build/winprogs/*.exe.

    python tools/make-winprogs.py          (on Windows, or inside WSL: then the Windows LLVM under /mnt/c is used)

Each program is freestanding C for --target=x86_64-pc-windows-msvc (no C runtime, no system headers: tests/win/src/win.h declares what they call), linked
by lld-link against import libraries that llvm-dlltool makes from tests/win/*.def. The outputs are never committed (tests/host/test_machine_tools.py fails
when an .exe is tracked). Prints the paths it built, one per line. The tools run with the repository as working directory and relative paths, which works
for the Windows executables both natively and through WSL's interop.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path("tests/win/src")
OUT = Path("build/winprogs")

# program -> (image base, extra import libraries)
PROGRAMS = {
    "hello": (0x140000000, []),
    "exitcode": (0x140000000, []),
    "args": (0x140000000, []),
    "heap": (0x140000000, []),
    "unused_import": (0x140000000, []),
    "called_import": (0x140000000, []),
    "reloc": (0x140000000, []),
    "reloc_low": (0x400000, []),          # reloc.c again, at the address of winrun's own program: winrun has to move it
    "otherdll": (0x140000000, ["user32"]),
    "files": (0x140000000, []),
}
SOURCE = {"reloc_low": "reloc"}
CRT = Path("tests/win/crt")
CRT_SRC = Path("tests/win/crtsrc")
CRT_PROGRAMS = ["fizzbuzz", "sort", "strings", "deep", "checksum", "teb", "fsops"]


def tool(name):
    """The LLVM tool NAME: $LLVM_DIR, the usual Windows install (natively or under /mnt/c in WSL), then PATH."""
    cands = []
    if os.environ.get("LLVM_DIR"):
        cands.append(Path(os.environ["LLVM_DIR"]))
    cands += [Path(r"C:\Program Files\LLVM\bin"), Path("/mnt/c/Program Files/LLVM/bin")]
    for d in cands:
        for n in (name + ".exe", name):
            if (d / n).is_file():
                return str(d / n)
    p = shutil.which(name) or shutil.which(name + ".exe")
    if p:
        return p
    sys.exit(f"make-winprogs: cannot find {name} (install LLVM or set LLVM_DIR)")


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit("make-winprogs: failed: " + " ".join(cmd) + "\n" + r.stdout + r.stderr)


def stale(out, deps):
    o = ROOT / out
    return not o.exists() or any((ROOT / d).stat().st_mtime > o.stat().st_mtime for d in deps)


def main():
    clang, lld, dlltool = tool("clang"), tool("lld-link"), tool("llvm-dlltool")
    (ROOT / OUT / "obj").mkdir(parents=True, exist_ok=True)
    for lib in ("kernel32", "user32"):
        d, l = Path("tests/win") / f"{lib}.def", OUT / f"{lib}.lib"
        if stale(l, [d]):
            run([dlltool, "-m", "i386:x86-64", "-d", d.as_posix(), "-l", l.as_posix()])
    for name, (base, libs) in PROGRAMS.items():
        src = SRC / f"{SOURCE.get(name, name)}.c"
        obj = OUT / "obj" / f"{name}.obj"
        exe = OUT / f"{name}.exe"
        if stale(obj, [src, SRC / "win.h", Path("tools/make-winprogs.py")]):
            run([clang, "--target=x86_64-pc-windows-msvc", "-ffreestanding", "-nostdinc", "-fno-builtin", "-fno-stack-protector", "-mno-stack-arg-probe",
                 "-O1", "-Wall", "-Werror", "-c", src.as_posix(), "-o", obj.as_posix()])
        if stale(exe, [obj, OUT / "kernel32.lib"]):
            run([lld, "/nologo", "/nodefaultlib", "/entry:start", "/subsystem:console", f"/base:{base:#x}", "/fixed:no", "/dynamicbase",
                 f"/out:{exe.as_posix()}", obj.as_posix(), (OUT / "kernel32.lib").as_posix(), *[(OUT / f"{l}.lib").as_posix() for l in libs]])
        print(ROOT / exe)
    # W2: C programs with main(argc, argv) on the mini C runtime (tests/win/crt): the compiler's stack probes stay on (__chkstk is the runtime's)
    crt_obj = OUT / "obj" / "crt.obj"
    flags = ["--target=x86_64-pc-windows-msvc", "-ffreestanding", "-nostdinc", "-fno-builtin", "-fno-stack-protector", "-O1", "-Wall", "-Werror", "-I", CRT.as_posix()]
    if stale(crt_obj, [CRT / "crt.c", CRT / "crt.h", Path("tools/make-winprogs.py")]):
        run([clang, *flags, "-c", (CRT / "crt.c").as_posix(), "-o", crt_obj.as_posix()])
    for name in CRT_PROGRAMS:
        src = CRT_SRC / f"{name}.c"
        obj = OUT / "obj" / f"crt_{name}.obj"
        exe = OUT / f"{name}.exe"
        if stale(obj, [src, CRT / "crt.h", Path("tools/make-winprogs.py")]):
            run([clang, *flags, "-c", src.as_posix(), "-o", obj.as_posix()])
        if stale(exe, [obj, crt_obj, OUT / "kernel32.lib"]):
            run([lld, "/nologo", "/nodefaultlib", "/entry:mainCRTStartup", "/subsystem:console", "/base:0x140000000", "/dynamicbase",
                 f"/out:{exe.as_posix()}", obj.as_posix(), crt_obj.as_posix(), (OUT / "kernel32.lib").as_posix()])
        print(ROOT / exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
