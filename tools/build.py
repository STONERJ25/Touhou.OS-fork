#!/usr/bin/env python3
"""Gensokyo build driver.

Builds, in order:
  loader   boot/*.c            -> build/BOOTX64.EFI        (PE/COFF, MS ABI, clang + lld-link)
  kernel   kernel/**/*.{c,asm} -> build/kernel.elf         (ELF64 higher half, clang + nasm + ld.lld)
  libc     user/libc/**        -> build/lib/libgap.a, crt0.o
  user     user/bin/*          -> build/user/bin/*         (static ELF executables)
  initrd   initrd/ + user bins -> build/initrd.img         (ustar archive)
  esp      everything above    -> build/esp/               (what QEMU boots as a FAT volume)

Usage: python tools/build.py [-v] [-j N] [--clean] [loader|kernel|user|initrd|esp ...]
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OBJ = BUILD / "obj"

LLVM_DIR = r"C:\Program Files\LLVM\bin"
NASM_DIR = os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM")


def find_tool(name, dirs):
    for d in dirs:
        for ext in ("", ".exe"):
            p = Path(d) / (name + ext)
            if p.is_file():
                return str(p)
    found = shutil.which(name)
    if found:
        return found
    sys.exit(f"build: cannot find '{name}' -- install LLVM/NASM or edit tools/build.py")


CLANG = find_tool("clang", [LLVM_DIR])
LLD_LINK = find_tool("lld-link", [LLVM_DIR])
LD_LLD = find_tool("ld.lld", [LLVM_DIR])
LLVM_AR = find_tool("llvm-ar", [LLVM_DIR])
OBJCOPY = find_tool("llvm-objcopy", [LLVM_DIR])
NASM = find_tool("nasm", [NASM_DIR])

COMMON_WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers"]

LOADER_CFLAGS = [
    "-target", "x86_64-unknown-windows", "-ffreestanding", "-fshort-wchar", "-fno-stack-protector",
    "-mno-red-zone", "-O2", "-std=gnu17", *COMMON_WARN, "-I", str(ROOT / "shared"),
]

KERNEL_CFLAGS = [
    "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=kernel", "-mgeneral-regs-only", "-fno-omit-frame-pointer",
    "-fno-strict-aliasing", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-O2", "-g", "-std=gnu17", *COMMON_WARN, "-DKERNEL=1",
    "-I", str(ROOT / "kernel" / "include"), "-I", str(ROOT / "shared"),
]

USER_CFLAGS = [
    "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-mno-red-zone",
    "-O2", "-g", "-std=gnu17", *COMMON_WARN,
    "-I", str(ROOT / "user" / "libc" / "include"), "-I", str(ROOT / "shared"),
]

VERBOSE = False


def rel(p):
    try:
        return str(Path(p).resolve().relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(p)


def parse_depfile(path):
    """Returns the prerequisite paths from a make-style .d file."""
    try:
        text = Path(path).read_text()
    except OSError:
        return []
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    idx = text.find(": ")
    if idx < 0:
        return []
    body = text[idx + 2:]
    deps = []
    for tok in re.split(r"(?<!\\)\s+", body.strip()):
        tok = tok.replace("\\ ", " ")
        if tok:
            deps.append(tok)
    return deps


def needs_build(out, depfile, cmd, inputs=()):
    out = Path(out)
    if not out.exists():
        return True
    stamp = out.with_name(out.name + ".cmd")
    cmd_text = "\n".join(cmd)
    if not stamp.exists() or stamp.read_text() != cmd_text:
        return True
    t = out.stat().st_mtime
    for dep in list(inputs) + (parse_depfile(depfile) if depfile else []):
        try:
            if os.stat(dep).st_mtime > t:
                return True
        except OSError:
            return True
    return False


def run(cmd, what):
    if VERBOSE:
        print("  $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        print(f"FAILED: {what}\n{out}", file=sys.stderr)
        raise SystemExit(1)
    return out


class Job:
    def __init__(self, src, obj, cmd, depfile, label):
        self.src, self.obj, self.cmd, self.depfile, self.label = src, obj, cmd, depfile, label


def c_job(src, group, flags):
    obj = OBJ / group / (rel(src) + ".o")
    dep = obj.with_name(obj.name + ".d")
    cmd = [CLANG, *flags, "-MMD", "-MF", str(dep), "-c", str(src), "-o", str(obj)]
    return Job(src, obj, cmd, dep, "CC")


def asm_job(src, group):
    obj = OBJ / group / (rel(src) + ".o")
    dep = obj.with_name(obj.name + ".d")
    cmd = [NASM, "-f", "elf64", "-g", "-F", "dwarf", "-I", str(src.parent) + os.sep,
           "-I", str(ROOT / "kernel" / "include") + os.sep, "-MD", str(dep), str(src), "-o", str(obj)]
    return Job(src, obj, cmd, dep, "ASM")


def build_jobs(jobs, nproc):
    todo = [j for j in jobs if needs_build(j.obj, j.depfile, j.cmd, [j.src])]
    if not todo:
        return False
    for j in todo:
        j.obj.parent.mkdir(parents=True, exist_ok=True)

    def work(j):
        r = subprocess.run(j.cmd, capture_output=True, text=True)
        return j, r

    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=nproc) as ex:
        for j, r in ex.map(work, todo):
            print(f"  {j.label:4} {rel(j.src)}")
            text = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0:
                errors.append((j, text))
                try:
                    j.obj.unlink()
                except OSError:
                    pass
            else:
                j.obj.with_name(j.obj.name + ".cmd").write_text("\n".join(j.cmd))
                if text.strip():
                    print(text.rstrip())
    if errors:
        for j, text in errors:
            print(f"FAILED: {rel(j.src)}\n{text}", file=sys.stderr)
        raise SystemExit(1)
    return True


def link(out, cmd, inputs, what):
    out = Path(out)
    stamp = out.with_name(out.name + ".cmd")
    cmd_text = "\n".join(cmd)
    if out.exists() and stamp.exists() and stamp.read_text() == cmd_text:
        t = out.stat().st_mtime
        if all(os.stat(i).st_mtime <= t for i in inputs):
            return False
    out.parent.mkdir(parents=True, exist_ok=True)
    print(f"  LINK {rel(out)}")
    run(cmd, what)
    stamp.write_text(cmd_text)
    return True


def sources(base, patterns):
    files = []
    for pat in patterns:
        files += sorted(Path(base).glob(pat))
    return files


# ------------------------------------------------------------------ loader

def build_loader(nproc):
    print("[loader]")
    srcs = sources(ROOT / "boot", ["*.c"])
    jobs = [c_job(s, "boot", LOADER_CFLAGS) for s in srcs]
    build_jobs(jobs, nproc)
    out = BUILD / "BOOTX64.EFI"
    cmd = [LLD_LINK, "/subsystem:efi_application", "/entry:efi_main", "/nodefaultlib", "/machine:x64",
           f"/out:{out}", *[str(j.obj) for j in jobs]]
    link(out, cmd, [j.obj for j in jobs], "link loader")


# ------------------------------------------------------------------ kernel

def build_kernel(nproc):
    print("[kernel]")
    csrc = sources(ROOT / "kernel", ["**/*.c"]) + sources(ROOT / "shared", ["*.c"])
    asrc = sources(ROOT / "kernel", ["**/*.asm"])
    jobs = [c_job(s, "kernel", KERNEL_CFLAGS) for s in csrc] + [asm_job(s, "kernel") for s in asrc]
    build_jobs(jobs, nproc)
    out = BUILD / "kernel.elf"
    script = ROOT / "kernel" / "linker.ld"
    cmd = [LD_LLD, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
           "-T", str(script), "-Map", str(BUILD / "kernel.map"), "-o", str(out), *[str(j.obj) for j in jobs]]
    link(out, cmd, [j.obj for j in jobs] + [script], "link kernel")


# ------------------------------------------------------------------ userland

def build_libc(nproc):
    libc = ROOT / "user" / "libc"
    if not libc.exists():
        return [], None
    csrc = sources(libc, ["**/*.c"])
    asrc = sources(libc, ["**/*.asm"])
    if not csrc and not asrc:
        return [], None
    print("[libc]")
    jobs = [c_job(s, "libc", USER_CFLAGS) for s in csrc] + [asm_job(s, "libc") for s in asrc]
    build_jobs(jobs, nproc)
    crt0 = [j.obj for j in jobs if j.src.name == "crt0.asm"]
    lib_objs = [j.obj for j in jobs if j.src.name != "crt0.asm"]
    lib = BUILD / "lib" / "libgap.a"
    if lib_objs:
        cmd = [LLVM_AR, "rcs", str(lib), *[str(o) for o in lib_objs]]
        if needs_build(lib, None, cmd, lib_objs):
            lib.parent.mkdir(parents=True, exist_ok=True)
            if lib.exists():
                lib.unlink()
            print(f"  AR   {rel(lib)}")
            run(cmd, "ar libgap")
            lib.with_name(lib.name + ".cmd").write_text("\n".join(cmd))
    return crt0, lib


def build_user(nproc):
    bindir = ROOT / "user" / "bin"
    if not bindir.exists():
        return []
    crt0, lib = build_libc(nproc)
    entries = sorted(p for p in bindir.iterdir() if (p.suffix == ".c" and p.is_file()) or p.is_dir())
    if not entries:
        return []
    print("[user]")
    script = ROOT / "user" / "user.ld"
    outputs = []
    all_jobs = []
    progs = []
    for e in entries:
        if e.is_file():
            name, srcs = e.stem, [e]
        else:
            name, srcs = e.name, sources(e, ["**/*.c"])
            if not srcs:
                continue
        jobs = [c_job(s, "user/" + name, USER_CFLAGS + ["-I", str(e if e.is_dir() else e.parent)]) for s in srcs]
        progs.append((name, jobs))
        all_jobs += jobs
    build_jobs(all_jobs, nproc)
    for name, jobs in progs:
        out = BUILD / "user" / "bin" / name
        cmd = [LD_LLD, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
               "-T", str(script), "-o", str(out), *map(str, crt0), *[str(j.obj) for j in jobs]]
        if lib:
            cmd.append(str(lib))
        ins = [j.obj for j in jobs] + crt0 + ([lib] if lib else []) + [script]
        link(out, cmd, ins, f"link {name}")
        outputs.append(out)
    return outputs


# ------------------------------------------------------------------ initrd / esp

def build_initrd(user_bins):
    print("[initrd]")
    out = BUILD / "initrd.img"
    items = []   # (arcname, path)
    for b in user_bins:
        items.append(("bin/" + b.name, b))
    root = ROOT / "initrd"
    if root.exists():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                items.append((p.relative_to(root).as_posix(), p))
    stamp = out.with_name(out.name + ".cmd")
    sig = "\n".join(f"{a}:{p.stat().st_mtime_ns}:{p.stat().st_size}" for a, p in items)
    if out.exists() and stamp.exists() and stamp.read_text() == sig:
        return out
    BUILD.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tf:
        for arc, p in items:
            ti = tf.gettarinfo(str(p), arcname=arc)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            ti.mtime = int(p.stat().st_mtime)
            ti.mode = 0o755 if arc.startswith("bin/") else 0o644
            with open(p, "rb") as f:
                tf.addfile(ti, f)
    stamp.write_text(sig)
    print(f"  TAR  {rel(out)} ({len(items)} files, {out.stat().st_size // 1024} KiB)")
    return out


def copy_if_changed(src, dst):
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and dst.stat().st_size == src.stat().st_size and dst.stat().st_mtime >= src.stat().st_mtime:
        return
    shutil.copyfile(src, dst)


def build_esp():
    print("[esp]")
    esp = BUILD / "esp"
    copy_if_changed(BUILD / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    kdst = esp / "kernel.elf"
    ksrc = BUILD / "kernel.elf"
    if not kdst.exists() or kdst.stat().st_mtime < ksrc.stat().st_mtime:
        run([OBJCOPY, "--strip-debug", str(ksrc), str(kdst)], "strip kernel")
    initrd = BUILD / "initrd.img"
    if initrd.exists():
        copy_if_changed(initrd, esp / "initrd.img")
    cmdline = esp / "cmdline.txt"
    if not cmdline.exists():
        cmdline.write_text("\n")
    print(f"  ESP  {rel(esp)}")


# ------------------------------------------------------------------ main

def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="*", help="loader kernel user initrd esp (default: all)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--clean", action="store_true", help="delete build/ first")
    args = ap.parse_args()
    VERBOSE = args.verbose

    if args.clean and BUILD.exists():
        shutil.rmtree(BUILD)
    want = set(args.targets) or {"loader", "kernel", "user", "initrd", "esp"}
    t0 = time.time()
    if "loader" in want:
        build_loader(args.jobs)
    if "kernel" in want:
        build_kernel(args.jobs)
    bins = []
    if "user" in want or "initrd" in want or "esp" in want:
        bins = build_user(args.jobs)
    if "initrd" in want or "esp" in want:
        build_initrd(bins)
    if "esp" in want:
        build_esp()
    print(f"build: done in {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
