#!/usr/bin/env python3
"""Builds the Linux root archive for TouhouOS (docs/COMPAT.md, "The Linux root") from the Ubuntu in WSL (or any Linux with the same programs):

    python tools/make-linux-root.py                     -> build/linux-inputs/linux-root.tar        (runs itself inside WSL on Windows)
    python tools/make-linux-root.py --gzip              -> build/linux-inputs/linux-root.tar.gz     (`linuxroot` reads both; the gzip one is about a third of the size)
    python tools/make-linux-root.py --out DIR           -> the archive and linux-root.txt (the file list) in DIR
    python tools/make-linux-root.py --programs ls,cat   -> only these programs (and the libraries they need)
    python tools/make-linux-root.py --python            -> build/linux-inputs/linux-root-python.tar (the default root plus Python, see below; --gzip works as above)
    python tools/make-linux-root.py --gcc               -> build/linux-inputs/linux-root-gcc.tar    (the default root plus the C compiler; --python --gcc: linux-root-python-gcc.tar)

The archive holds a small distribution tree: the dynamic linker, the C library and the libraries the chosen programs need (found with ldd), bash, dash as /bin/sh and a set of
coreutils and friends, the usr-merge symlinks (/bin -> usr/bin ...), a few files under /etc, and empty mount points. On the Dell the archive lives on the Human Village volume
(/HumanVillage/linux-root.tar) and `linuxroot` unpacks it into a tmpfs mounted on /linux the first time a Linux program is wanted.

--python (L3, docs/COMPAT.md "State: L3") adds the interpreter of this machine (/usr/bin/python3.12 and the python3 link), its standard library (/usr/lib/python3.12 without the
test suite, idlelib, tkinter, turtledemo, ensurepip, the pydoc topics and every __pycache__: Python compiles what it imports and, as root, writes the result back into the
tree) and the extension modules of lib-dynload that do not drag in a large tree, together with the shared libraries ldd names for them. The archive is its own file, so
the default root does not change. What was left out, and why, is printed at the end of a run and written in the file list.

--gcc (L3) adds the C compiler of this machine: the driver (gcc, cc), the compiler proper (cc1), the assembler and the linker (as, ld) with collect2, the plug-in and wrapper the
link step passes to ld, the start-up files and libraries a dynamically linked program is built with (crt1.o, crti.o, libgcc.a, the linker scripts of libc.so and libm.so ...), and the
headers a C file includes (/usr/include without the C++ and driver trees, and the compiler's own). `gcc -c hello.c` gives the same object file as the same gcc on the machine the
archive was made on. --python and --gcc can be combined (linux-root-python-gcc.tar). Not included: g++ and the C++ library, the static C library (libc.a: no `gcc -static`), the
sanitizers, the Fortran, OpenMP and LTO compilers.

The files are third-party data (GNU, glibc, Ubuntu): they are never committed and never go into an image; everything this script makes is under build/, which git ignores. Every
file is stored as a plain regular file or symlink owned by root (there are no hard links, so any tar reader can unpack it); the times are fixed so that the same programs give
the same archive.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# What goes in. Edit these tables to change the root.

# programs, by name: taken from /usr/bin (the usr-merged /bin) of this machine
PROGRAMS = ["bash", "dash", "ls", "cat", "echo", "env", "uname", "grep", "sed", "sort", "wc", "head", "tail", "tr", "cut", "printf", "true", "false", "id", "ln", "mkdir",
            "rm", "cp", "mv", "stat", "date", "sleep",
            # a few more that scripts and the tests use
            "basename", "dirname", "tee", "readlink", "touch", "chmod", "rmdir", "test", "[", "seq", "expr", "pwd", "whoami", "hostname", "uniq", "tac", "od", "mktemp",
            "realpath", "dd", "printenv",
            # the programs that read /proc (docs/COMPAT.md, "A Linux-shaped /proc"): procps (libproc2 and, for the sake of `ps -o unit`, libsystemd with its libraries) and util-linux's lscpu
            "ps", "free", "uptime", "pidof", "pgrep", "pkill", "top", "vmstat", "nproc", "lscpu", "kill"]
# the C library and its run-time companions: always present, whether or not ldd names them (the C library loads the nss ones by itself, with dlopen)
LIBRARIES = ["ld-linux-x86-64.so.2", "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libresolv.so.2", "libnss_files.so.2", "libnss_dns.so.2",
             "libutil.so.1", "libgcc_s.so.1"]
# the usr-merge links of a current Ubuntu
SYMLINKS = {"bin": "usr/bin", "sbin": "usr/sbin", "lib": "usr/lib", "lib64": "usr/lib64"}
USR_MERGED = ("bin", "sbin", "lib", "lib64")
# empty directories: (path, mode)
DIRECTORIES = [("proc", 0o755), ("dev", 0o755), ("sys", 0o755), ("run", 0o755), ("tmp", 0o1777), ("root", 0o700), ("home", 0o755), ("mnt", 0o755), ("opt", 0o755),
               ("srv", 0o755), ("var", 0o755), ("var/tmp", 0o1777), ("var/log", 0o755), ("etc", 0o755), ("usr/bin", 0o755), ("usr/sbin", 0o755), ("usr/lib", 0o755),
               ("usr/lib64", 0o755), ("usr/local", 0o755), ("usr/local/bin", 0o755), ("usr/share", 0o755), ("usr/lib/x86_64-linux-gnu", 0o755)]
# files that are made up, not copied: path -> content. The users are the ones of TouhouOS's own /etc/passwd (uid 0 and the four homes).
ETC = {
    "etc/passwd": "root:x:0:0:root:/root:/bin/bash\nreimu:x:1000:1000:Reimu Hakurei:/home/reimu:/bin/bash\nmarisa:x:1001:1001:Marisa Kirisame:/home/marisa:/bin/bash\n"
                  "sakuya:x:1002:1002:Sakuya Izayoi:/home/sakuya:/bin/bash\nsumireko:x:1003:1003:Sumireko Usami:/home/sumireko:/bin/bash\n",
    "etc/group": "root:x:0:\nshrine:x:100:reimu,marisa,sakuya\nreimu:x:1000:\nmarisa:x:1001:\nsakuya:x:1002:\nsumireko:x:1003:\n",
    "etc/nsswitch.conf": "passwd: files\ngroup: files\nshadow: files\nhosts: files dns\nnetworks: files\nprotocols: files\nservices: files\n",
    "etc/hosts": "127.0.0.1 localhost\n::1 localhost\n",
    "etc/hostname": "gensokyo\n",
    "etc/shells": "/bin/sh\n/bin/bash\n/bin/dash\n",
    # linuxroot replaces this with the content of the real one (/Hakurei/etc/resolv.conf) when it unpacks: an archive cannot know the network it will run on
    "etc/resolv.conf": "nameserver 10.0.2.3\n",
}
# files copied from the machine the archive is made on: path -> source (read through symlinks, stored as plain files)
COPIES = {"etc/os-release": "/etc/os-release", "usr/lib/os-release": "/etc/os-release",
          # the terminal descriptions top (ncurses) looks for in /usr/share/terminfo
          "usr/share/terminfo/d/dumb": "/usr/share/terminfo/d/dumb", "usr/share/terminfo/l/linux": "/usr/share/terminfo/l/linux",
          "usr/share/terminfo/x/xterm": "/usr/share/terminfo/x/xterm", "usr/share/terminfo/v/vt100": "/usr/share/terminfo/v/vt100"}

FIXED_TIME = 1767225600           # 2026-01-01: the archive does not depend on when it was made

# --python: what is taken from the Python of this machine (Ubuntu 24.04: python3.12, a static interpreter with the common modules built in, a standard library in
# /usr/lib/python3.12 and the extension modules in its lib-dynload)
PYTHON_PRUNE_ANYWHERE = {"__pycache__", "test", "tests", "idle_test"}                  # directories never copied, at any depth
PYTHON_PRUNE_TOP = {"idlelib", "tkinter", "turtledemo", "ensurepip", "config-3.12-x86_64-linux-gnu", "pydoc_data"}      # in the top directory of the library only
PYTHON_PRUNE_FILES = {"turtle.py", "sitecustomize.py"}                                  # turtle needs tkinter; sitecustomize is a link into /etc/python3.12 (Debian's, empty of use)
# extension modules left out, by name prefix: the test modules, tkinter (X11 and Tk: tens of libraries), curses (ncurses and the terminfo database), the Berkeley and GNU dbm
# bindings (libdb is 2 MB and nothing here uses it; dbm falls back to its pure Python module), the sound card binding
PYTHON_DYNLOAD_SKIP = ("_test", "_xx", "xx", "_ctypes_test", "_tkinter", "_curses", "_dbm", "_gdbm", "ossaudiodev")
# directories a Python on Ubuntu puts on sys.path and that are absent here: empty, so that site.py finds them where it looks
PYTHON_EMPTY_DIRS = ["usr/lib/python3/dist-packages", "usr/local/lib/python3.12/dist-packages"]
# data a program may want and a base system always has: the C.UTF-8 locale (Python switches the C locale to it, PEP 538; 400 KB) and the time zone database (zoneinfo, 630 KB; the
# two names that are links into /etc, which the root does not have, are left out)
PYTHON_DATA_DIRS = ["/usr/lib/locale/C.utf8", "/usr/share/zoneinfo"]
PYTHON_DATA_SKIP = {"localtime", "posixrules"}

# --gcc: the C compiler of this machine (Ubuntu 24.04: gcc 13.3 and binutils 2.42). Programs from /usr/bin (the links gcc and cc lead to the driver, as and ld to the GNU ones), the
# parts of the compiler in /usr/libexec/gcc/<triplet>/<version>, the compiler's own files and headers in /usr/lib/gcc/<triplet>/<version>, the start-up files and the libraries
# of a dynamic link in /usr/lib/<triplet>, and the headers a C file includes in /usr/include.
GCC_PROGRAMS = ["gcc", "cc", "as", "ld", "ld.bfd"]
GCC_LIBEXEC = ["cc1", "collect2", "lto-wrapper", "liblto_plugin.so"]
GCC_LIB_FILES = ["crtbegin.o", "crtbeginS.o", "crtbeginT.o", "crtend.o", "crtendS.o", "libgcc.a", "libgcc_eh.a", "libgcc_s.so"]
GCC_ARCH_FILES = ["crt1.o", "crti.o", "crtn.o", "Scrt1.o", "rcrt1.o", "libc.so", "libc_nonshared.a", "libm.so", "libm.a", "libmvec.so", "libmvec.so.1", "libpthread.a", "libdl.a",
                  "librt.a", "libutil.a", "libanl.a"]
# the headers: whole directories of /usr/include (the top-level files are copied too); the C++ headers (12 MB), the driver, sound and rdma trees and Python's are not a C file's business
GCC_INCLUDE_DIRS = ["x86_64-linux-gnu", "linux", "asm-generic", "arpa", "net", "netinet", "protocols", "rpcsvc"]

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------


def in_wsl():
    """Runs this script again inside WSL's Ubuntu when it was started on Windows. Returns its exit status."""
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        sys.exit("make-linux-root: no wsl.exe; run this script on a Linux with the programs you want (or install WSL with an Ubuntu)")

    def wsl_path(p):
        p = Path(p).resolve()
        return f"/mnt/{p.drive[0].lower()}/" + "/".join(p.parts[1:])

    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--out" and i + 1 < len(args):
            args[i + 1] = wsl_path(args[i + 1])
    return subprocess.call([wsl, "-e", "python3", wsl_path(__file__), *args])


def merged(path):
    """The usr-merged spelling of an absolute path: /lib/x86_64-linux-gnu/libc.so.6 is usr/lib/x86_64-linux-gnu/libc.so.6 in the archive."""
    p = path.lstrip("/")
    first = p.split("/", 1)[0]
    return "usr/" + p if first in USR_MERGED else p


class Tree:
    """The archive being assembled: path (no leading slash) -> ("d", mode) | ("f", mode, bytes) | ("l", target)."""

    def __init__(self):
        self.entries = {}

    def _parents(self, path):
        parts = path.split("/")
        for i in range(1, len(parts)):
            self.entries.setdefault("/".join(parts[:i]), ("d", 0o755))

    def dir(self, path, mode=0o755):
        path = path.strip("/")
        self._parents(path)
        self.entries[path] = ("d", mode)

    def file(self, path, data, mode):
        path = path.strip("/")
        self._parents(path)
        self.entries[path] = ("f", mode, data)

    def link(self, path, target):
        path = path.strip("/")
        self._parents(path)
        self.entries[path] = ("l", target)

    def add_host_file(self, host_path):
        """Copies a file of this machine to the same place in the tree (usr-merged), with the symlinks along its name as symlinks: libfoo.so.1 -> libfoo.so.1.2.3 stays one."""
        cur = host_path
        for _ in range(10):
            at = merged(cur)
            if not os.path.islink(cur):
                break
            target = os.readlink(cur)
            self.link(at, target)
            cur = target if os.path.isabs(target) else os.path.normpath(os.path.join(os.path.dirname(cur), target))
        real = os.path.realpath(cur)
        mode = os.stat(real).st_mode & 0o7777 & ~0o022              # nobody writes to the system files but root
        self.file(merged(cur), Path(real).read_bytes(), mode)


def ldd(path):
    """The libraries a program needs, as the dynamic linker of this machine resolves them: {soname: path}."""
    r = subprocess.run(["ldd", path], capture_output=True, text=True)
    libs = {}
    for line in r.stdout.splitlines():
        line = line.strip()
        if "=>" in line:
            name, _, rest = line.partition("=>")
            if rest.strip().startswith("/"):
                libs[name.strip()] = rest.strip().split(" ")[0]
        elif line.startswith("/"):                                  # the dynamic linker itself: "/lib64/ld-linux-x86-64.so.2 (0x...)"
            p = line.split(" ")[0]
            libs[os.path.basename(p)] = p
    return libs


def find_library(name):
    for d in ("/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def add_python(t, libs, left_out):
    """--python: the interpreter, its standard library and the extension modules of this machine. The shared libraries that ldd names for the interpreter and for each
    module that went in are added to `libs` (soname -> path; build_tree copies them), and what was left out is appended to `left_out` as text."""
    exe = os.path.realpath("/usr/bin/python3")
    if not os.path.isfile(exe):
        sys.exit("make-linux-root: --python: /usr/bin/python3 is not here (apt install python3 on the machine the archive is made on)")
    lib = "/usr/lib/" + os.path.basename(exe)                       # /usr/lib/python3.12
    t.add_host_file("/usr/bin/python3")                              # the link python3 -> python3.12 and the interpreter itself
    libs.update(ldd(exe))
    skipped_dirs, skipped_mods, skipped_bytes = set(), [], 0
    for dirpath, dirnames, filenames in os.walk(lib):
        rel = os.path.relpath(dirpath, lib)
        top = rel == "."
        keep = []
        for d in sorted(dirnames):
            if d in PYTHON_PRUNE_ANYWHERE or (top and d in PYTHON_PRUNE_TOP):
                skipped_dirs.add(d)
                skipped_bytes += sum(os.path.getsize(os.path.join(a, b)) for a, _, bs in os.walk(os.path.join(dirpath, d)) for b in bs if os.path.isfile(os.path.join(a, b)))
            elif os.path.islink(os.path.join(dirpath, d)):
                t.link(os.path.join(dirpath, d).lstrip("/"), os.readlink(os.path.join(dirpath, d)))
            else:
                keep.append(d)
        dirnames[:] = keep
        t.dir(dirpath.lstrip("/"), 0o755)
        for f in sorted(filenames):
            full = os.path.join(dirpath, f)
            at = full.lstrip("/")
            if f.endswith((".pyc", ".pyo")) or (top and f in PYTHON_PRUNE_FILES):
                if top and f in PYTHON_PRUNE_FILES:
                    skipped_mods.append(f)
                continue
            if rel == "lib-dynload" and f.startswith(PYTHON_DYNLOAD_SKIP):
                skipped_mods.append(f.split(".")[0])
                continue
            if os.path.islink(full):
                target = os.readlink(full)
                resolved = target if os.path.isabs(target) else os.path.normpath(os.path.join(dirpath, target))
                if resolved.startswith(lib + "/") and os.path.exists(resolved):
                    t.link(at, target)                              # a link inside the library (the sysconfig data under its other name)
                else:
                    skipped_mods.append(f)
                continue
            if not os.path.isfile(full):
                continue
            t.file(at, Path(full).read_bytes(), os.stat(full).st_mode & 0o7777 & ~0o022)
            if rel == "lib-dynload":
                libs.update(ldd(full))
    for d in PYTHON_EMPTY_DIRS:
        t.dir(d)
    for d in PYTHON_DATA_DIRS:
        for dirpath, dirnames, filenames in os.walk(d):
            dirnames.sort()
            t.dir(merged(dirpath), 0o755)
            for f in sorted(filenames):
                if f not in PYTHON_DATA_SKIP:
                    t.add_host_file(os.path.join(dirpath, f))
    for f in ("/etc/services", "/etc/protocols"):                  # socket.getservbyname("http"), getprotobyname("tcp"): the netbase tables (16 KB)
        if os.path.exists(f):
            t.file(f.lstrip("/"), Path(os.path.realpath(f)).read_bytes(), 0o644)
    left_out.append(f"directories {', '.join(sorted(skipped_dirs))} of the standard library ({skipped_bytes / 1e6:.1f} MB)")
    left_out.append(f"extension modules and files {', '.join(sorted(set(skipped_mods)))}")


def add_gcc(t, libs, left_out):
    """--gcc: the C compiler of this machine and everything `gcc -c` and a dynamic `gcc hello.c -o hello` need (see GCC_* above). The shared libraries ldd names for each program are
    added to `libs`; what could not be found is appended to `left_out`."""
    driver = os.path.realpath("/usr/bin/gcc")
    if not os.path.isfile(driver):
        sys.exit("make-linux-root: --gcc: /usr/bin/gcc is not here (apt install gcc on the machine the archive is made on)")
    version = subprocess.run(["gcc", "-dumpfullversion"], capture_output=True, text=True).stdout.strip().split(".")[0]          # 13
    triplet = subprocess.run(["gcc", "-dumpmachine"], capture_output=True, text=True).stdout.strip()                           # x86_64-linux-gnu
    libexec = f"/usr/libexec/gcc/{triplet}/{version}"
    gcclib = f"/usr/lib/gcc/{triplet}/{version}"
    archlib = f"/usr/lib/{triplet}"
    missing = []
    for name in GCC_PROGRAMS:
        p = os.path.join("/usr/bin", name)
        if not os.path.exists(p):
            missing.append(name)
            continue
        t.add_host_file(p)
        libs.update(ldd(os.path.realpath(p)))
    for name in GCC_LIBEXEC:
        p = os.path.join(libexec, name)
        if not os.path.exists(p):
            missing.append(p)
            continue
        t.add_host_file(p)
        libs.update(ldd(os.path.realpath(p)))
    for base, names in ((gcclib, GCC_LIB_FILES), (archlib, GCC_ARCH_FILES)):
        for name in names:
            p = os.path.join(base, name)
            if os.path.exists(p):
                t.add_host_file(p)
            else:
                missing.append(p)
    n_headers = 0
    for dirpath, dirnames, filenames in os.walk(os.path.join(gcclib, "include")):               # the compiler's own headers: stddef.h, stdarg.h, the *intrin.h files
        dirnames[:] = sorted(dirnames)
        for f in sorted(filenames):
            t.add_host_file(os.path.join(dirpath, f))
            n_headers += 1
    for f in sorted(os.listdir("/usr/include")):                                                  # /usr/include/*.h
        p = os.path.join("/usr/include", f)
        if os.path.isfile(p):
            t.add_host_file(p)
            n_headers += 1
    for d in GCC_INCLUDE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join("/usr/include", d)):
            dirnames[:] = sorted(dirnames)
            for f in sorted(filenames):
                t.add_host_file(os.path.join(dirpath, f))
                n_headers += 1
    t.dir("usr/local/include")
    t.dir("usr/local/lib")
    left_out.append(f"gcc {version} for {triplet}: {n_headers} headers; not included: g++ and the C++ headers and library, libc.a (no `gcc -static`), the sanitizers, the Fortran, OpenMP and LTO compilers")
    if missing:
        left_out.append("gcc files that were not found on this machine: " + ", ".join(missing))


def build_tree(programs, python=False, gcc=False):
    t = Tree()
    for path, mode in DIRECTORIES:
        t.dir(path, mode)
    for path, target in SYMLINKS.items():
        t.link(path, target)
    skipped, libs, left_out = [], {}, []
    for name in programs:
        path = os.path.join("/usr/bin", name)
        if not os.path.exists(path):
            skipped.append(name)
            continue
        t.add_host_file(path)
        libs.update(ldd(os.path.realpath(path)))
    for name in LIBRARIES:
        p = find_library(name)
        if p:
            libs.setdefault(name, p)
        else:
            skipped.append(name)
    if python:
        add_python(t, libs, left_out)
    if gcc:
        add_gcc(t, libs, left_out)
    for path in sorted(set(libs.values())):
        t.add_host_file(path)
    if "dash" in programs and os.path.exists("/usr/bin/dash"):
        t.link("usr/bin/sh", "dash")                                # /bin/sh is dash, as on Ubuntu
    for path, content in ETC.items():
        t.file(path, content.encode(), 0o644)
    for at, src in COPIES.items():
        t.file(at, Path(os.path.realpath(src)).read_bytes(), 0o644)
    return t, skipped, left_out


def write_tar(tree, out, gzip):
    with tarfile.open(out, "w:gz" if gzip else "w", format=tarfile.GNU_FORMAT) as tf:
        for path in sorted(tree.entries):
            e = tree.entries[path]
            ti = tarfile.TarInfo(path)
            ti.mtime = FIXED_TIME
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            if e[0] == "d":
                ti.type, ti.mode = tarfile.DIRTYPE, e[1]
                tf.addfile(ti)
            elif e[0] == "l":
                ti.type, ti.mode, ti.linkname = tarfile.SYMTYPE, 0o777, e[1]
                tf.addfile(ti)
            else:
                ti.type, ti.mode, ti.size = tarfile.REGTYPE, e[1], len(e[2])
                tf.addfile(ti, io.BytesIO(e[2]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(ROOT / "build" / "linux-inputs"), help="directory for linux-root.tar (default build/linux-inputs)")
    ap.add_argument("--gzip", action="store_true", help="compress the archive (linux-root.tar.gz)")
    ap.add_argument("--programs", help="comma-separated program names instead of the table at the top of this script")
    ap.add_argument("--python", action="store_true", help="add Python and its standard library (linux-root-python.tar)")
    ap.add_argument("--gcc", action="store_true", help="add the C compiler, the assembler, the linker and the headers (linux-root-gcc.tar)")
    args = ap.parse_args()
    if os.name == "nt":
        return in_wsl()
    programs = args.programs.split(",") if args.programs else PROGRAMS
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    tree, skipped, left_out = build_tree(programs, python=args.python, gcc=args.gcc)
    stem = "linux-root" + ("-python" if args.python else "") + ("-gcc" if args.gcc else "")        # each combination is an archive of its own: the default root stays what it was
    name = stem + (".tar.gz" if args.gzip else ".tar")
    write_tar(tree, out / name, args.gzip)
    total = sum(len(e[2]) for e in tree.entries.values() if e[0] == "f")
    files = sum(1 for e in tree.entries.values() if e[0] == "f")
    with open(out / (stem + ".txt"), "w", encoding="utf-8", newline="\n") as f:
        for path in sorted(tree.entries):
            e = tree.entries[path]
            f.write(f"{e[0]} {oct(e[1]) if e[0] != 'l' else '->'} {len(e[2]) if e[0] == 'f' else 0} {path}{' -> ' + e[1] if e[0] == 'l' else ''}\n")
        for line in left_out:
            f.write(f"# left out: {line}\n")
    digest = hashlib.sha256((out / name).read_bytes()).hexdigest()[:16]
    print(f"make-linux-root: {name}: {files} files, {len(tree.entries)} entries, {total / 1e6:.1f} MB of data, {(out / name).stat().st_size / 1e6:.1f} MB archive (sha256 {digest}...)")
    if skipped:
        print(f"make-linux-root: not found on this machine, left out: {', '.join(skipped)}")
    for line in left_out:
        print(f"make-linux-root: left out: {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
