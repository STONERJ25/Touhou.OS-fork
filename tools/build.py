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
import struct
import subprocess
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OBJ = BUILD / "obj"

# Where the tools come from, in order: TOUHOU_LLVM_DIR (the folder with clang, lld-link, ld.lld, llvm-ar, llvm-objcopy,
# llvm-nm) and TOUHOU_NASM (the nasm program or its folder), then the PATH, then the Windows install folders this was
# written with. docs/LINUX.md lists the Linux packages.
WINDOWS_LLVM_DIR = r"C:\Program Files\LLVM\bin"
WINDOWS_NASM_DIR = os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM")
EXE = ".exe" if os.name == "nt" else ""


def _in_dir(d, name):
    p = Path(d) / (name + EXE)
    return str(p) if p.is_file() else None


def find_tool(name, env_var, windows_dir):
    env = os.environ.get(env_var)
    if env:
        if Path(env).is_file():
            return env
        found = _in_dir(env, name)
        if found:
            return found
        sys.exit(f"build: {env_var}={env} has no '{name}'")
    found = shutil.which(name)
    if found:
        return found
    if os.name == "nt":
        found = _in_dir(windows_dir, name)
        if found:
            return found
    sys.exit(f"build: cannot find '{name}' -- install LLVM/NASM or set {env_var} (see docs/LINUX.md)")


def find_llvm(name):
    return find_tool(name, "TOUHOU_LLVM_DIR", WINDOWS_LLVM_DIR)


CLANG = find_llvm("clang")
LLD_LINK = find_llvm("lld-link")
LD_LLD = find_llvm("ld.lld")
LLVM_AR = find_llvm("llvm-ar")
OBJCOPY = find_llvm("llvm-objcopy")
NASM = find_tool("nasm", "TOUHOU_NASM", WINDOWS_NASM_DIR)
LLVM_NM = find_llvm("llvm-nm")

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
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-math-errno",
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
    def __init__(self, src, obj, cmd, depfile, label, extra=()):
        self.src, self.obj, self.cmd, self.depfile, self.label = src, obj, cmd, depfile, label
        self.extra = list(extra)


def c_job(src, group, flags):
    obj = OBJ / group / (rel(src) + ".o")
    dep = obj.with_name(obj.name + ".d")
    cmd = [CLANG, *flags, "-MMD", "-MF", str(dep), "-c", str(src), "-o", str(obj)]
    return Job(src, obj, cmd, dep, "CC")


def asm_job(src, group):
    obj = OBJ / group / (rel(src) + ".o")
    dep = obj.with_name(obj.name + ".d")
    cmd = [NASM, "-f", "elf64", "-g", "-F", "dwarf", "-I", str(src.parent) + os.sep,
           "-I", str(ROOT / "kernel" / "include") + os.sep, "-I", str(BUILD / "gen") + os.sep,
           "-MD", str(dep), str(src), "-o", str(obj)]
    extra = [BUILD / "gen" / "ksyms.bin"] if src.name == "ksyms.asm" else []
    return Job(src, obj, cmd, dep, "ASM", extra)


def build_jobs(jobs, nproc):
    todo = [j for j in jobs if needs_build(j.obj, j.depfile, j.cmd, [j.src] + j.extra)]
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

def kernel_symbols(elf):
    """Text symbols of the linked kernel as a ksyms blob: header, (addr, name_off) entries, names."""
    out = run([LLVM_NM, "-n", "--defined-only", str(elf)], "nm kernel")
    entries = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t", "W", "w"):
            entries.append((int(parts[0], 16), parts[2]))
    names = bytearray()
    table = []
    last = None
    for addr, name in entries:
        if addr == last:
            continue
        last = addr
        table.append((addr, len(names)))
        names += name.encode() + b"\0"
    blob = struct.pack("<II", len(table), 0)
    blob += b"".join(struct.pack("<QII", a, o, 0) for a, o in table)
    return blob + bytes(names)


def build_kernel(nproc):
    print("[kernel]")
    csrc = sources(ROOT / "kernel", ["**/*.c"]) + sources(ROOT / "shared", ["*.c"])
    asrc = sources(ROOT / "kernel", ["**/*.asm"])
    gen = BUILD / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    ksyms_bin = gen / "ksyms.bin"
    if not ksyms_bin.exists():
        ksyms_bin.write_bytes(struct.pack("<II", 0, 0))
    jobs = [c_job(s, "kernel", KERNEL_CFLAGS) for s in csrc] + [asm_job(s, "kernel") for s in asrc]
    build_jobs(jobs, nproc)
    out = BUILD / "kernel.elf"
    script = ROOT / "kernel" / "linker.ld"
    cmd = [LD_LLD, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
           "-T", str(script), "-Map", str(BUILD / "kernel.map"), "-o", str(out), *[str(j.obj) for j in jobs]]
    inputs = [j.obj for j in jobs] + [script]
    link(out, cmd, inputs, "link kernel")
    # Embed the symbol table: it sits in the last segment, so addresses seen now stay valid after the relink.
    table = kernel_symbols(out)
    if not ksyms_bin.exists() or ksyms_bin.read_bytes() != table:
        ksyms_bin.write_bytes(table)
        build_jobs(jobs, nproc)
        link(out, cmd, inputs, "relink kernel with symbols")
        if kernel_symbols(out) != table:
            print("warning: symbol addresses moved after embedding ksyms", file=sys.stderr)


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


LIB_ORDER = ["kappa", "fairy"]          # link order: a library may depend on the ones after it (and on libgap, always last)
LIB_EXTRA_SOURCES = {"kappa": ["shared/font8x16.c", "shared/yinyang.c"]}


def user_lib_includes():
    base = ROOT / "user" / "lib"
    return [p / "include" for p in sorted(base.iterdir()) if (p / "include").is_dir()] if base.exists() else []


def build_userlibs(nproc):
    """Static libraries under user/lib/<name>/{include,src}: built to build/lib/lib<name>.a, linked into every program."""
    base = ROOT / "user" / "lib"
    if not base.exists():
        return []
    names = sorted(p.name for p in base.iterdir() if p.is_dir())
    names.sort(key=lambda n: (LIB_ORDER.index(n) if n in LIB_ORDER else len(LIB_ORDER), n))
    inc = []
    for p in user_lib_includes():
        inc += ["-I", str(p)]
    groups, all_jobs = [], []
    for n in names:
        csrc = sources(base / n, ["src/**/*.c"]) + [ROOT / s for s in LIB_EXTRA_SOURCES.get(n, [])]
        if not csrc:
            continue
        jobs = [c_job(s, f"userlib/{n}", USER_CFLAGS + inc) for s in csrc]
        groups.append((n, jobs))
        all_jobs += jobs
    if not all_jobs:
        return []
    print("[userlibs]")
    build_jobs(all_jobs, nproc)
    libs = []
    for n, jobs in groups:
        objs = [j.obj for j in jobs]
        lib = BUILD / "lib" / f"lib{n}.a"
        cmd = [LLVM_AR, "rcs", str(lib), *[str(o) for o in objs]]
        if needs_build(lib, None, cmd, objs):
            lib.parent.mkdir(parents=True, exist_ok=True)
            if lib.exists():
                lib.unlink()
            print(f"  AR   {rel(lib)}")
            run(cmd, f"ar lib{n}")
            lib.with_name(lib.name + ".cmd").write_text("\n".join(cmd))
        libs.append(lib)
    return libs


KAPPABOX_APPLETS = []


def generate_applet_table(srcs):
    """kappabox applets register themselves with APPLET(name) { ... }; this writes the dispatch table."""
    names = []
    for s in srcs:
        for m in re.finditer(r"^APPLET\((\w+)\)", s.read_text(encoding="utf-8", errors="replace"), re.M):
            names.append(m.group(1))
    names = sorted(set(names))
    KAPPABOX_APPLETS[:] = names
    lines = ["/* GENERATED by tools/build.py -- the list of kappabox applets. */", "#include <stddef.h>", ""]
    lines += [f"int applet_{n}(int argc, char **argv);" for n in names]
    lines += ["", "struct KbApplet { const char *name; int (*fn)(int, char **); };",
              "const struct KbApplet kb_applets[] = {"]
    lines += [f'    {{ "{n}", applet_{n} }},' for n in names]
    lines += ["    { NULL, NULL }", "};", f"const int kb_napplets = {len(names)};", ""]
    gen = BUILD / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    out = gen / "kappabox_table.c"
    text = "\n".join(lines)
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    return out


def build_user(nproc):
    """Builds every program under user/bin (-> /bin), user/sbin (-> /sbin) and user/test (-> /usr/lib/kernel-tests, which is not
    in anybody's PATH: the kernel's fuzzers and stress programs live there). Returns [(install_dir, path)]."""
    crt0, lib = build_libc(nproc)
    userlibs = build_userlibs(nproc)
    lib_inc = []
    for p in user_lib_includes():
        lib_inc += ["-I", str(p)]
    script = ROOT / "user" / "user.ld"
    outputs = []
    all_jobs = []
    progs = []
    for kind in ("bin", "sbin", "test"):
        d = ROOT / "user" / kind
        if not d.exists():
            continue
        for e in sorted(p for p in d.iterdir() if (p.suffix == ".c" and p.is_file()) or p.is_dir()):
            if e.is_file():
                name, srcs = e.stem, [e]
            else:
                name, srcs = e.name, sources(e, ["**/*.c"])
                if not srcs:
                    continue
                if name == "kappabox":
                    srcs.append(generate_applet_table(srcs))
            jobs = [c_job(s, f"user/{kind}/{name}", USER_CFLAGS + lib_inc + ["-I", str(e if e.is_dir() else e.parent)]) for s in srcs]
            progs.append((kind, name, jobs))
            all_jobs += jobs
    if not progs:
        return []
    print("[user]")
    build_jobs(all_jobs, nproc)
    for kind, name, jobs in progs:
        out = BUILD / "user" / kind / name
        cmd = [LD_LLD, "-nostdlib", "-static", "-z", "max-page-size=4096", "-z", "noexecstack",
               "-T", str(script), "-o", str(out), *map(str, crt0), *[str(j.obj) for j in jobs], *map(str, userlibs)]
        if lib:
            cmd.append(str(lib))
        ins = [j.obj for j in jobs] + crt0 + userlibs + ([lib] if lib else []) + [script]
        link(out, cmd, ins, f"link {kind}/{name}")
        outputs.append((kind, out))
    return outputs


# ------------------------------------------------------------------ initrd / esp

# Programs that run with their owner's authority (setuid root) and files that only root may read.
SPECIAL_MODES = {"bin/eiki": 0o4755, "bin/su": 0o4755, "bin/passwd": 0o4755, "sbin/pwcheck": 0o4755, "Hakurei/etc/shadow": 0o600}
EXTRA_LINKS = [("bin/sudo", "eiki"), ("bin/sh", "gapsh"),       # #!/bin/sh scripts (and system()/popen() callers) find a shell
               ("bin/fsck.fat", "fsck"), ("bin/fsck.vfat", "fsck")]


def build_initrd(user_bins):
    print("[initrd]")
    out = BUILD / "initrd.img"
    items = []   # (arcname, path)
    stripped_dir = BUILD / "user-stripped"
    for kind, b in user_bins:
        sdst = stripped_dir / kind / b.name
        if not sdst.exists() or sdst.stat().st_mtime < b.stat().st_mtime:
            sdst.parent.mkdir(parents=True, exist_ok=True)
            run([OBJCOPY, "--strip-all", str(b), str(sdst)], f"strip {b.name}")
        arc = f"ScarletDevilMansion/lib/kernel-tests/{b.name}" if kind == "test" else f"{kind}/{b.name}"
        items.append((arc, sdst))
    root = ROOT / "initrd"
    if root.exists():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                items.append((p.relative_to(root).as_posix(), p))
    have = {a for a, _ in items}
    links = [(f"bin/{n}", "kappabox") for n in KAPPABOX_APPLETS if f"bin/{n}" not in have]
    links += [(a, t) for a, t in EXTRA_LINKS if a not in have]
    stamp = out.with_name(out.name + ".cmd")
    sig = "\n".join(f"{a}:{p.stat().st_mtime_ns}:{p.stat().st_size}" for a, p in items) + "\n" + "\n".join(f"{a}>{t}" for a, t in links)
    if out.exists() and stamp.exists() and stamp.read_text() == sig:
        return out
    BUILD.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tf:
        for arc, p in items:
            ti = tf.gettarinfo(str(p), arcname=arc)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            ti.mtime = int(p.stat().st_mtime)
            ti.mode = SPECIAL_MODES.get(arc, 0o755 if arc.startswith(("bin/", "sbin/", "ScarletDevilMansion/lib/kernel-tests/")) else 0o644)
            with open(p, "rb") as f:
                tf.addfile(ti, f)
        for arc, target in links:
            ti = tarfile.TarInfo(arc)
            ti.type = tarfile.SYMTYPE
            ti.linkname = target
            ti.mode = 0o777
            ti.mtime = int(time.time())
            tf.addfile(ti)
    stamp.write_text(sig)
    print(f"  TAR  {rel(out)} ({len(items)} files, {len(links)} links, {out.stat().st_size // 1024} KiB)")
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
