#!/usr/bin/env python3
"""Counts what a set of Linux programs ask of the kernel (docs/COMPAT.md, "The census"). Run it inside WSL or on any x86-64 Linux with gcc:

    wsl.exe -e python3 <checkout>/tools/linuxcensus.py [--out FILE]

It builds tools/linuxcensus.c (a ptrace-based call counter), makes three small test programs of its own (a static glibc hello, a hello that makes raw
system calls without any libc, a one-line C file to compile), runs each program of PROGRAMS under the counter and prints a Markdown report: every system
call with the number of programs that use it and the calls made, then the flag values seen for the calls that carry them.
The programs are test inputs (what a typical Linux userland does); nothing of them is copied into the tree.
"""
import argparse
import collections
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

HELLO_C = '#include <stdio.h>\nint main(void) { puts("hi"); return 0; }\n'
RAW_C = r'''
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
void _start(void) {
    sys3(1, 1, (long)"hi\n", 3);          /* write */
    sys3(231, 0, 0, 0);                   /* exit_group */
    for (;;) { }
}
'''

BITS = {
    "mmap prot": [(1, "READ"), (2, "WRITE"), (4, "EXEC")],
    "mmap flags": [(0x1, "SHARED"), (0x2, "PRIVATE"), (0x10, "FIXED"), (0x20, "ANONYMOUS"), (0x40, "GROWSDOWN"), (0x100, "DENYWRITE"), (0x800, "DENYWRITE"),
                   (0x4000, "NORESERVE"), (0x8000, "POPULATE"), (0x20000, "STACK"), (0x40000, "HUGETLB")],
    "clone flags": [(0x100, "VM"), (0x200, "FS"), (0x400, "FILES"), (0x800, "SIGHAND"), (0x2000, "PTRACE"), (0x4000, "VFORK"), (0x8000, "PARENT"), (0x10000, "THREAD"),
                    (0x80000, "SETTLS"), (0x100000, "PARENT_SETTID"), (0x200000, "CHILD_CLEARTID"), (0x1000000, "CHILD_SETTID")],
    "openat flags": [(0x1, "WRONLY"), (0x2, "RDWR"), (0x40, "CREAT"), (0x80, "EXCL"), (0x100, "NOCTTY"), (0x200, "TRUNC"), (0x400, "APPEND"), (0x800, "NONBLOCK"),
                     (0x10000, "DIRECTORY"), (0x20000, "NOFOLLOW"), (0x80000, "CLOEXEC"), (0x200000, "PATH")],
}
FUTEX_OPS = {0: "WAIT", 1: "WAKE", 3: "REQUEUE", 4: "CMP_REQUEUE", 5: "WAKE_OP", 6: "LOCK_PI", 7: "UNLOCK_PI", 9: "WAIT_BITSET", 10: "WAKE_BITSET"}
WHAT = {(9, 2): "mmap prot", (9, 3): "mmap flags", (10, 2): "mprotect prot", (28, 2): "madvise advice", (56, 0): "clone flags", (202, 1): "futex op", (16, 1): "ioctl request",
        (72, 1): "fcntl command", (157, 0): "prctl option", (158, 0): "arch_prctl code", (257, 2): "openat flags", (2, 1): "open flags", (41, 0): "socket domain",
        (41, 1): "socket type", (228, 0): "clock_gettime clock", (302, 1): "prlimit64 resource", (318, 2): "getrandom flags", (13, 0): "rt_sigaction signal",
        (334, 3): "rseq flags", (435, 0): "clone3 (pointer)"}


def decode(kind, val):
    if kind in ("mprotect prot",):
        kind = "mmap prot"
    if kind == "open flags":
        kind = "openat flags"
    if kind == "futex op":
        return FUTEX_OPS.get(val & 0x7f, hex(val & 0x7f)) + ("|PRIVATE" if val & 0x80 else "") + ("|REALTIME" if val & 0x100 else "")
    if kind in BITS:
        names = [n for b, n in BITS[kind] if val & b]
        if kind == "openat flags":
            names.insert(0, ("RDONLY", "WRONLY", "RDWR")[val & 3] if val & 3 != 3 else "?")
            names = [n for n in names if n not in ("WRONLY", "RDWR") or n == names[0]]
        return "|".join(names) if names else "0"
    return ""


def programs(tmp):
    return {
        "true": ["/bin/true"],
        "ls": ["/bin/ls", "-l", "/usr/bin"],
        "cat": ["/bin/cat", "/etc/os-release"],
        "sh pipeline": ["/bin/sh", "-c", "echo hi | /usr/bin/wc -c; ls / > %s/ls.out; rm %s/ls.out" % (tmp, tmp)],
        "python (nothing)": ["/usr/bin/python3", "-c", "pass"],
        "python (threads, sockets, hashing)": ["/usr/bin/python3", "-c",
                                               "import threading,time,os,socket,json,hashlib\n"
                                               "t=threading.Thread(target=time.sleep,args=(0.01,)); t.start(); t.join()\n"
                                               "s=socket.socket(); s.close(); print(len(os.listdir('/usr')), hashlib.sha256(b'x').hexdigest()[:8])"],
        "gcc -c hello.c": ["/usr/bin/gcc", "-O2", "-c", "%s/hello.c" % tmp, "-o", "%s/hello.o" % tmp],
        "static glibc hello": ["%s/static" % tmp],
        "raw hello (no libc)": ["%s/raw" % tmp],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="also write the report to this file")
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="census-")
    (Path(tmp) / "hello.c").write_text(HELLO_C)
    (Path(tmp) / "raw.c").write_text(RAW_C)
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True)
    for cmd in (["gcc", "-O2", "-o", f"{tmp}/census", str(HERE / "linuxcensus.c")],
                ["gcc", "-O2", "-static", "-o", f"{tmp}/static", f"{tmp}/hello.c"],
                ["gcc", "-O2", "-static", "-nostdlib", "-o", f"{tmp}/raw", f"{tmp}/raw.c"]):
        r = run(cmd)
        if r.returncode:
            sys.exit(f"linuxcensus: {' '.join(cmd)} failed:\n{r.stderr}")
    calls = collections.defaultdict(dict)                       # name -> program -> count
    nums = {}
    hist = collections.defaultdict(lambda: collections.Counter())    # (kind) -> value -> count
    totals = {}
    for prog, cmd in programs(tmp).items():
        r = run([f"{tmp}/census", *cmd])
        total = 0
        for line in r.stdout.splitlines():
            f = line.split()
            if f[0] == "S":
                nums[f[2]] = int(f[1])
                calls[f[2]][prog] = int(f[3])
                total += int(f[3])
            elif f[0] == "H":
                kind = WHAT.get((int(f[1]), int(f[3])), f"{f[2]} arg{f[3]}")
                hist[kind][int(f[4], 16)] += int(f[5])
        totals[prog] = total
        print(f"[{prog}] {total} calls ({r.stderr.strip().splitlines()[-1] if r.stderr.strip() else 'no message'})", file=sys.stderr)
    out = []
    names = list(programs(tmp))
    out.append("## System calls seen\n")
    out.append(f"{len(calls)} different calls in {len(names)} programs. Programs: " + ", ".join(f"{n} ({totals[n]} calls)" for n in names) + "\n")
    out.append("| call | number | programs | calls in all | used by |")
    out.append("|---|---|---|---|---|")
    for name in sorted(calls, key=lambda n: (-len(calls[n]), -sum(calls[n].values()), n)):
        users = calls[name]
        out.append(f"| `{name}` | {nums[name]} | {len(users)} | {sum(users.values())} | {', '.join(u for u in names if u in users)} |")
    out.append("\n## Flag values seen\n")
    for kind in sorted(hist):
        vals = hist[kind].most_common(12)
        out.append(f"* **{kind}**: " + ", ".join(f"`{hex(v)}`" + (f" ({decode(kind, v)})" if decode(kind, v) else "") + f" x{n}" for v, n in vals))
    text = "\n".join(out) + "\n"
    if args.out:
        Path(args.out).write_text(text)
    print(text)


if __name__ == "__main__":
    main()
