#!/usr/bin/env python3
"""Builds the Linux root archive for TouhouOS (docs/COMPAT.md, "The Linux root") from the Ubuntu in WSL (or any Linux with the same programs):

    python tools/make-linux-root.py                     -> build/linux-inputs/linux-root.tar        (runs itself inside WSL on Windows)
    python tools/make-linux-root.py --gzip              -> build/linux-inputs/linux-root.tar.gz     (`linuxroot` reads both; the gzip one is about a third of the size)
    python tools/make-linux-root.py --out DIR           -> the archive and linux-root.txt (the file list) in DIR
    python tools/make-linux-root.py --programs ls,cat   -> only these programs (and the libraries they need)

The archive holds a small distribution tree: the dynamic linker, the C library and the libraries the chosen programs need (found with ldd), bash, dash as /bin/sh and a set of
coreutils and friends, the usr-merge symlinks (/bin -> usr/bin ...), a few files under /etc, and empty mount points. On the Dell the archive lives on the Human Village volume
(/HumanVillage/linux-root.tar) and `linuxroot` unpacks it into a tmpfs mounted on /linux the first time a Linux program is wanted.

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
            "realpath", "dd", "printenv"]
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
COPIES = {"etc/os-release": "/etc/os-release", "usr/lib/os-release": "/etc/os-release"}

FIXED_TIME = 1767225600           # 2026-01-01: the archive does not depend on when it was made

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


def build_tree(programs):
    t = Tree()
    for path, mode in DIRECTORIES:
        t.dir(path, mode)
    for path, target in SYMLINKS.items():
        t.link(path, target)
    skipped, libs = [], {}
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
    for path in sorted(set(libs.values())):
        t.add_host_file(path)
    if "dash" in programs and os.path.exists("/usr/bin/dash"):
        t.link("usr/bin/sh", "dash")                                # /bin/sh is dash, as on Ubuntu
    for path, content in ETC.items():
        t.file(path, content.encode(), 0o644)
    for at, src in COPIES.items():
        t.file(at, Path(os.path.realpath(src)).read_bytes(), 0o644)
    return t, skipped


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
    args = ap.parse_args()
    if os.name == "nt":
        return in_wsl()
    programs = args.programs.split(",") if args.programs else PROGRAMS
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    tree, skipped = build_tree(programs)
    name = "linux-root.tar.gz" if args.gzip else "linux-root.tar"
    write_tar(tree, out / name, args.gzip)
    total = sum(len(e[2]) for e in tree.entries.values() if e[0] == "f")
    files = sum(1 for e in tree.entries.values() if e[0] == "f")
    with open(out / "linux-root.txt", "w", encoding="utf-8", newline="\n") as f:
        for path in sorted(tree.entries):
            e = tree.entries[path]
            f.write(f"{e[0]} {oct(e[1]) if e[0] != 'l' else '->'} {len(e[2]) if e[0] == 'f' else 0} {path}{' -> ' + e[1] if e[0] == 'l' else ''}\n")
    digest = hashlib.sha256((out / name).read_bytes()).hexdigest()[:16]
    print(f"make-linux-root: {name}: {files} files, {len(tree.entries)} entries, {total / 1e6:.1f} MB of data, {(out / name).stat().st_size / 1e6:.1f} MB archive (sha256 {digest}...)")
    if skipped:
        print(f"make-linux-root: not found on this machine, left out: {', '.join(skipped)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
