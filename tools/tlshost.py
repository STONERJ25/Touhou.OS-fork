#!/usr/bin/env python3
"""Builds libtls (user/lib/tls/src) into a Windows DLL so that the build machine can test it directly with Python.

The same sources go into the operating system's libtls.a; here they are compiled freestanding for x86_64-pc-windows-msvc
together with tools/tlshost/hostlib.c (mem*, str*, malloc) and loaded with ctypes. tests/case_tls.py checks every primitive
against Python's `cryptography` and `hashlib`, runs the protocol against Python's `ssl` module, and checks the certificate
code against the Mozilla roots of `certifi` -- all without booting QEMU.

  python tools/tlshost.py            build the DLL (build/tlshost/tlshost.dll) and list what it exports
"""
import ctypes
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "user" / "lib" / "tls" / "src"
OUTDIR = ROOT / "build" / "tlshost"
LLVM = Path(r"C:\Program Files\LLVM\bin")

CFLAGS = ["-target", "x86_64-pc-windows-msvc", "-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe", "-fno-builtin-memcpy",
          "-O2", "-g0", "-std=gnu17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
          "-DTLS_HOST=1", "-I", str(ROOT / "user" / "libc" / "include"), "-I", str(ROOT / "shared"), "-I", str(SRC), "-I", str(SRC.parent / "include")]


def tool(name):
    p = LLVM / (name + ".exe")
    if p.exists():
        return str(p)
    sys.exit(f"tlshost: cannot find {name}")


def sources():
    files = sorted(p for p in SRC.glob("*.c") if not p.name.startswith("os_"))
    return files + [ROOT / "tools" / "tlshost" / "hostlib.c"]


def exported_names():
    """Every tlsc_*, tls_*, x509_* and asn1_* function declared in the headers (they are all exported from the DLL)."""
    names = set()
    for hdr in list(SRC.glob("*.h")) + list((SRC.parent / "include").glob("*.h")):
        text = hdr.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for m in re.finditer(r"\b((?:tlsc|tls|x509|asn1|pem)_\w+)\s*\(", text):
            names.add(m.group(1))
    names.add("tlshost_reset")
    return sorted(names)


def build(force=False):
    OUTDIR.mkdir(parents=True, exist_ok=True)
    objs = []
    newest = max(p.stat().st_mtime for p in list(SRC.glob("*")) + list(SRC.parent.glob("include/*")) + [Path(__file__)] + sources())
    dll = OUTDIR / "tlshost.dll"
    for src in sources():
        obj = OUTDIR / (src.stem + ".o")
        objs.append(obj)
        if force or not obj.exists() or obj.stat().st_mtime < newest:
            r = subprocess.run([tool("clang"), *CFLAGS, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"tlshost: compile failed for {src.name}:\n{r.stdout}{r.stderr}")
            if r.stderr.strip():
                print(r.stderr.rstrip())
    if force or not dll.exists() or dll.stat().st_mtime < max(o.stat().st_mtime for o in objs):
        defined = set()
        nm = subprocess.run([tool("llvm-nm"), "--defined-only", "-g", *map(str, objs)], capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TtDdBbRr":
                defined.add(parts[2])
        exports = [f"/export:{n}" for n in exported_names() if n in defined]
        cmd = [tool("lld-link"), "/dll", "/noentry", "/nodefaultlib", "/machine:x64", f"/out:{dll}", *exports, *map(str, objs)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"tlshost: link failed:\n{r.stdout}{r.stderr}")
    return dll


_lib = None


def load(force=False):
    global _lib
    if _lib is None or force:
        _lib = ctypes.CDLL(str(build(force)))
    return _lib


if __name__ == "__main__":
    path = build(force="--force" in sys.argv)
    print("built", path)
    print(len(exported_names()), "names to export")
