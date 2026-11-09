"""L2 of docs/COMPAT.md: position-independent and dynamically linked Linux programs, run from a Linux root that the user provides.

The programs are ordinary C built with gcc on a real Linux (tests/linux/src, tools/linux-oracle.py): the test downloads them into the guest from a host HTTP server. What a real
Linux prints for them is stored in tests/linux/golden and compared line by line. The Linux root is built from the Ubuntu in WSL by tools/make-linux-root.py, downloaded as a tar
archive, and unpacked by `linuxroot` into a tmpfs mounted on /linux, which is "/" for every Linux program that is started afterwards. Nothing of the Linux side (the programs, the
C library, the archive) is committed: it is all built under build/linux-inputs at run time, and without WSL (or a Linux with gcc) these tests print SKIP and pass.

    python tools/qemu-slot.py -- python tests/run_all.py linux_dyn
    LINUX_ONLY=dyn_hello,dyn_dlopen python tests/run_all.py linux_dyn_loader      limits the programs; LINUX_TRACE=1 logs every call; LINUX_LIMIT=300 sets the time limit
"""
import contextlib
import difflib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu, BUILD, scaled
from case_linux import Inputs, build_inputs, fetch, run, compare_with_linux, TRACE, INPUTS, GOLDEN

ROOT_PROMPT = r"root@gensokyo \S+ # "
TOOLS = Path(__file__).resolve().parent.parent / "tools"


def skip_reason():
    """Why the Linux side cannot be built here (None when it can)."""
    if sys.platform == "win32":
        if not shutil.which("wsl.exe"):
            return "WSL is not installed: the test programs and the Linux root are built with gcc on a Linux"
    elif not shutil.which("gcc"):
        return "gcc is not installed"
    return None


def build_root():
    """build/linux-inputs/linux-root.tar from the Ubuntu in WSL (tools/make-linux-root.py), unless it is newer than the script."""
    tar = INPUTS / "linux-root.tar"
    script = TOOLS / "make-linux-root.py"
    if tar.exists() and tar.stat().st_mtime > script.stat().st_mtime:
        return tar
    r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True)
    assert r.returncode == 0 and tar.exists(), f"building the Linux root failed:\n{r.stdout}\n{r.stderr}"
    return tar


def root_run(vm, cmd, timeout=90):
    """One command line in the guest's root shell: (output, status)."""
    out, rc = test_shell.run(vm, cmd, timeout=timeout, prompt=ROOT_PROMPT)
    return out.replace("\r", ""), rc


def fetch_root(vm, port, name):
    """Downloads build/linux-inputs/NAME into /tmp of the guest (the network comes up a little after the prompt, so it retries) and makes it executable."""
    deadline = time.time() + scaled(180)
    while True:
        out, rc = root_run(vm, f"fetch -q -O /tmp/{name} http://10.0.2.2:{port}/{name}", timeout=180)
        if rc == 0:
            break
        assert time.time() < deadline, f"could not download {name}: {out!r}"
        time.sleep(3)
    root_run(vm, f"chmod +x /tmp/{name}")


class World:
    """A guest booted as root with a network card, the programs downloaded into /tmp and (root=True) the Linux root unpacked into /linux.

        with World("t-linux-x", ["dyn_hello"]) as w:
            out, rc = w.run("linux /tmp/dyn_hello")
    """

    def __init__(self, name, files=(), root=True, mem="1G", cmdline="text user=root", extra_files=()):
        self.name, self.files, self.root, self.mem, self.cmdline = name, list(files), root, mem, cmdline
        self.extra = list(extra_files)

    def __enter__(self):
        build_inputs()
        if self.root:
            build_root()
        self.stack = contextlib.ExitStack()
        try:
            self.srv = self.stack.enter_context(Inputs())
            self.vm = self.stack.enter_context(Qemu(cmdline=self.cmdline + TRACE, name=self.name, net="virtio-net-pci", mem=self.mem))
            self.vm.expect(ROOT_PROMPT, timeout=300)
            for n in self.files + self.extra:
                fetch_root(self.vm, self.srv.port, n)
            if self.root:
                fetch_root(self.vm, self.srv.port, "linux-root.tar")
                t0 = time.time()
                out, rc = root_run(self.vm, "linuxroot /tmp/linux-root.tar", timeout=300)
                assert rc == 0, f"linuxroot failed ({rc}): {out}"
                self.unpack_seconds = time.time() - t0
                root_run(self.vm, "rm /tmp/linux-root.tar")                 # the archive in RAM is not needed any more
        except BaseException:
            self.stack.close()
            raise
        return self

    def __exit__(self, *exc):
        self.stack.close()
        return False

    def run(self, cmd, timeout=90):
        return root_run(self.vm, cmd, timeout=int(os.environ.get("LINUX_LIMIT", timeout)))


def selected(names):
    only = os.environ.get("LINUX_ONLY")
    return [n for n in names if not only or n in only.split(",")]


def compare_all(w, cases, failures):
    """cases: golden name -> (command line, time limit). Runs each and compares it with the stored output of a real Linux."""
    for n in selected(cases):
        cmd, limit = cases[n]
        out, rc = w.run(cmd, timeout=limit)
        try:
            compare_with_linux(n, out, rc)
        except AssertionError as e:
            failures.append(str(e))


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the loader

@test
def linux_dyn_static_pie():
    """A static-PIE executable (ET_DYN with no interpreter) is mapped at a base of the kernel's choosing, relocates itself, and sees the program headers, the thread-local
    template and the relocated pointers it should: the same output as on a real Linux"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    with World("t-linux-spie", ["dyn_hello_spie"], root=False, cmdline="text user=root") as w:
        compare_all(w, {"dyn_hello_spie": ("FOO=bar linux /tmp/dyn_hello_spie alpha beta", 90)}, failures)
    assert not failures, "\n\n".join(failures)


@test
def linux_dyn_loader():
    """Dynamically linked programs from the Linux root: a position-independent executable and a non-PIE executable with an interpreter (ld.so is mapped, relocates itself, maps
    libc.so.6 from the root and hands over), dlopen of a library through LD_LIBRARY_PATH and from the system directories with constructors, destructors and thread-local
    storage, fork + exec and posix_spawn of another dynamic program, system() and popen(): the same output as on a real Linux"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    cases = {
        "dyn_hello": ("FOO=bar linux /tmp/dyn_hello alpha beta", 90),
        "dyn_hello_nopie": ("FOO=bar linux /tmp/dyn_hello_nopie alpha beta", 90),
        "dyn_dlopen": ("LD_LIBRARY_PATH=/tmp linux /tmp/dyn_dlopen", 120),
        "dyn_spawn": ("linux /tmp/dyn_spawn", 180),
    }
    with World("t-linux-loader", ["dyn_hello", "dyn_hello_nopie", "dyn_dlopen", "dyn_spawn", "libdynplug.so"]) as w:
        compare_all(w, cases, failures)
    assert not failures, "\n\n".join(failures)


@test
def linux_dyn_ld_debug_shape():
    """LD_DEBUG=libs makes the dynamic linker say what it does: it looks for libc.so.6 in the system directories of the root, initializes it, then the program, and hands
    control over (the lines have the shape of a real Linux's, without the numbers)"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with World("t-linux-lddebug", ["dyn_hello"]) as w:
        out, rc = w.run("LD_DEBUG=libs linux /tmp/dyn_hello 2>&1", timeout=120)
        assert rc == 3, f"status {rc}\n{out}"
        text = re.sub(r"^\s*\d+:\t", "", out, flags=re.M)
        want = ["find library=libc.so.6 [0]; searching", "trying file=/lib/x86_64-linux-gnu/libc.so.6", "calling init: /lib/x86_64-linux-gnu/libc.so.6",
                "initialize program: /tmp/dyn_hello", "transferring control: /tmp/dyn_hello", "calling fini:"]
        at = 0
        for line in want:
            found = text.find(line, at)
            assert found >= 0, f"LD_DEBUG=libs lacks {line!r} (after offset {at}):\n{out}"
            at = found + len(line)
        assert "hello from a dynamic PIE program" in out, out


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the C library corpus again, dynamically linked

@test
def linux_dyn_glibc_corpus():
    """The programs of the L1 corpus (a tour of the C library, directories, system information), built as dynamically linked PIEs and run from the Linux root: the same output
    as the static ones give on a real Linux"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    cases = {"dyn_libc": ("FOO=bar linux /tmp/dyn_libc alpha beta", 180), "dyn_dir": ("linux /tmp/dyn_dir", 180), "dyn_sys": ("linux /tmp/dyn_sys", 180)}
    with World("t-linux-corpus", ["dyn_libc", "dyn_dir", "dyn_sys"]) as w:
        compare_all(w, cases, failures)
    assert not failures, "\n\n".join(failures)


@test
def linux_dyn_signals_and_threads():
    """Signals and POSIX threads through the dynamically linked C library (handlers and frames, the alternate stack, SA_RESTART, pthread_create with dynamic TLS, futexes,
    tgkill): the same output as on a real Linux"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    cases = {"dyn_signals": ("linux /tmp/dyn_signals", 300), "dyn_threads": ("linux /tmp/dyn_threads", 300)}
    with World("t-linux-sigthr", ["dyn_signals", "dyn_threads"]) as w:
        for n in selected(cases):
            cmd, limit = cases[n]
            out, rc = w.run(cmd, timeout=limit)
            try:
                kernel = compare_with_linux(n, out, rc, ("[Eiki]",) if n == "dyn_signals" else ())
                if n == "dyn_signals":
                    for what in ("crossed the Boundary at 0x10", "was exorcised by signal 15", "was exorcised by signal 6"):
                        assert any(what in l for l in kernel), f"{n}: the kernel log lacks {what!r}; it had {kernel}"
            except AssertionError as e:
                failures.append(str(e))
    assert not failures, "\n\n".join(failures)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# memory mapping

@test
def linux_dyn_mmap_tour():
    """The mapping calls of a dynamic linker one by one: private file mappings at offsets, a PROT_NONE reservation filled with MAP_FIXED, partial munmap, mprotect, the flags a
    loader passes, madvise(MADV_DONTNEED) and mremap, and the errors of bad arguments: the same output as on a real Linux"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    with World("t-linux-mmap", ["map_tour"], root=False) as w:
        compare_all(w, {"map_tour": ("linux /tmp/map_tour", 120)}, failures)
    assert not failures, "\n\n".join(failures)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the shell and the tools of the root, against a real Linux

SCRIPTS = {
    "sh_files": ("files and directories: ls, cp, mv, ln, rm, mkdir, touch, chmod, stat, readlink, cat, dd, od", 180),
    "sh_pipes": ("pipelines, here-documents, redirection, arithmetic, loops, functions, exit statuses, the text tools", 180),
    "sh_env": ("the environment, /proc/self/fd, /dev, /etc/os-release, the paths of the root", 180),
}


@test
def linux_dyn_shell_and_tools():
    """/bin/sh (dash) and /bin/bash with the coreutils, grep and sed of the root run three scripts (files and directories; pipelines and the shell language; the environment,
    /proc and /dev) in a directory the test makes: every line is what the same script prints on a real Linux. A user without privileges can run Linux programs too"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    failures = []
    names = selected(SCRIPTS)
    with World("t-linux-shell", [s + ".sh" for s in names], extra_files=["root_paths"]) as w:
        t0 = time.time()
        out, rc = w.run("linux /bin/ls /", timeout=120)
        first_ls = time.time() - t0
        assert rc == 0 and "usr" in out.split() and "lib64" in out.split(), f"linux /bin/ls /: {rc} {out!r}"
        print(f"     (linux /bin/ls /: {first_ls:.1f} s, including the shell round trip; unpacking the root took {w.unpack_seconds:.1f} s)")
        out, rc = w.run("linux /bin/cat /etc/os-release", timeout=60)
        assert rc == 0 and out.startswith('PRETTY_NAME="Ubuntu'), f"cat /etc/os-release: {rc} {out!r}"
        out, rc = w.run("linux /bin/sh -c 'echo $((1+2))'", timeout=60)
        assert rc == 0 and out == "3\n", f"sh -c: {rc} {out!r}"
        for shell in ("/bin/sh", "/bin/bash"):
            for n in names:
                # through a pipe, as the oracle runs it: ls prints one name per line only when its output is not a terminal. The kernel names a call it lacks once on the console
                # ("[Outside World] ... is not implemented", e.g. the socket that the name service switch tries): those lines are set aside.
                cmd = f"rm -rf /tmp/wk; mkdir /tmp/wk; (cd /tmp/wk && linux /usr/bin/env -i PATH=/usr/bin:/bin {shell} /tmp/{n}.sh 2>&1 | cat)"
                out, rc = w.run(cmd, timeout=SCRIPTS[n][1])
                try:
                    compare_with_linux(n, out, rc, ("[Outside World]",))
                except AssertionError as e:
                    failures.append(f"({shell}) {e}")
        # an unprivileged user: the files of the root are owned by root and readable, /tmp is shared, the user and group names come from the root's /etc
        out, rc = w.run("su reimu -c 'linux /usr/bin/id'", timeout=60)
        assert rc == 0 and out.startswith("uid=1000(reimu) gid=1000(reimu)"), f"id as reimu: {rc} {out!r}"
        out, rc = w.run("su reimu -c 'linux /bin/sh -c \"echo x > /etc/passwd\"' 2>&1", timeout=60)
        assert rc != 0 and "ermission denied" in out, f"a user must not write root's /etc/passwd: {rc} {out!r}"
        out, rc = w.run("su reimu -c 'linux /bin/sh -c \"cd /tmp && echo made by reimu > rp_user && ls -l rp_user\"'; ls -l /tmp/rp_user", timeout=60)
        assert rc == 0 and out.count("reimu") >= 2, f"{rc} {out!r}"
    assert not failures, "\n\n".join(failures)


@test
def linux_dyn_interactive_bash():
    """An interactive bash from the Linux root, typed to over the console: readline switches the terminal to its own settings with Linux's struct termios (translated to the
    native one by the Linux personality), echoes what is typed once, edits the line (a backspace takes the last character back), runs the command and gives the prompt back"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with World("t-linux-bash", []) as w:
        vm = w.vm
        vm.send("linux /bin/bash --norc --noprofile\n")
        vm.expect(r"bash-5\.\d+# ", timeout=120)
        mark = len(vm.text())
        vm.send("echo interactive-$((6*7))\n")
        vm.expect(r"\ninteractive-42\r?\n", timeout=60)
        vm.expect(r"bash-5\.\d+# ", timeout=60)
        typed = vm.text()[mark:]
        assert typed.count("echo interactive-") == 1, f"the command line should be echoed once (readline's echo), not twice:\n{typed!r}"
        vm.send("echo abcX\x7fY\n")                             # a backspace (DEL) takes the X back
        vm.expect(r"\nabcY\r?\n", timeout=60)
        vm.expect(r"bash-5\.\d+# ", timeout=60)
        vm.send("exit\n")
        vm.expect(ROOT_PROMPT, timeout=60)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the root itself

ROOT_RULES = """--- where '..' leads
/.. is the same directory as /
/tmp/.. is the same directory as /
/dev/.. is the same directory as /
/proc/.. is the same directory as /
/etc/../.. is the same directory as /
/usr/bin/../.. is the same directory as /
/tmp/../etc is the same directory as /etc
--- what exists
/etc/os-release: exists
/usr/bin/ls: exists
/lib64/ld-linux-x86-64.so.2: exists
/HumanVillage: No such file or directory
/Hakurei: No such file or directory
/ScarletDevilMansion: No such file or directory
/BambooForest: No such file or directory
/linux: No such file or directory
/dev/null: exists
/proc/self: exists
the top directory holds: bin dev etc home lib lib64 mnt opt proc root run sbin srv sys tmp usr var
/bin is a symbolic link to: usr/bin
--- the working directory
getcwd at the start: /tmp
/proc/self/cwd: /tmp
after chdir(/etc): /etc
after chdir(..): /
after chdir(../../..): /
from /tmp, ../etc/os-release starts with: PRETTY_NAME=
a relative path in the root: libc.so.6 is here
--- symbolic links are read inside the root
/tmp/rp_link -> /etc/passwd reads: root:x:0:0:root:/root:/bin/bash
it is the root's passwd file (Reimu Hakurei is in it): yes
--- the machine's own directories
created /tmp/rp_marker: yes
/dev/zero gives 4 bytes
/proc/self/exe: /tmp/root_paths
/proc/uptime can be read: yes
uname: Linux x86_64
"""


@test
def linux_dyn_root_rules():
    """The Linux root as a Linux program sees it: its own tree is "/", ".." stops at the top (also from /tmp, /dev and /proc), an absolute symbolic link is read inside the root,
    /proc, /dev and /tmp are the machine's own, the native directories do not exist, the working directory is kept when it is visible and moved to "/" when it is not, native
    programs still see everything, and the root cannot be mounted twice or emptied while a Linux program lives in it"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with World("t-linux-rules", ["root_paths"]) as w:
        out, rc = w.run("cd /tmp && linux /tmp/root_paths", timeout=60)
        assert rc == 0, f"root_paths: {rc}\n{out}"
        if out != ROOT_RULES:
            diff = "\n".join(difflib.unified_diff(ROOT_RULES.splitlines(), out.splitlines(), "the rules", "TouhouOS", lineterm="", n=0))
            raise AssertionError(f"root_paths differs from the rules of the Linux root:\n{diff}")
        out, rc = w.run("cat /tmp/rp_marker; ls /HumanVillage /Hakurei | head -3", timeout=30)
        assert rc == 0 and "made by a Linux program" in out and "reimu" in out, f"a native program sees the file the Linux program made in /tmp, and its own directories: {out!r}"
        # the working directory outside the root becomes "/" of the root
        out, rc = w.run("cd /HumanVillage/reimu && linux /bin/pwd", timeout=30)
        assert rc == 0 and out == "/\n", f"pwd from a native directory: {rc} {out!r}"
        out, rc = w.run("cd /tmp && linux /bin/pwd", timeout=30)
        assert rc == 0 and out == "/tmp\n", f"pwd from /tmp: {rc} {out!r}"
        # -r: the program runs in the real root (a Linux binary kept at a native path)
        out, rc = w.run("linux -r /ScarletDevilMansion/share/linux-tests/lnx_hello", timeout=30)
        assert rc == 0 and "hello from a Linux program" in out, f"linux -r: {rc} {out!r}"
        out, rc = w.run("linux /ScarletDevilMansion/share/linux-tests/lnx_hello", timeout=30)
        assert rc != 0 and "No such file" in out, f"in the root a native path does not exist: {rc} {out!r}"
        # the root stays while a program lives in it, and cannot be mounted twice
        out, rc = w.run("linuxroot /tmp/nothing.tar", timeout=30)
        assert rc != 0 and "already holds" in out, f"{rc} {out!r}"
        out, rc = w.run("linux /bin/sleep 5 & sleep 1; linuxroot -d; wait", timeout=60)
        assert "still running" in out and "stays mounted" in out, f"linuxroot -d while a program runs: {out!r}"
        out, rc = w.run("linuxroot -d; linuxroot; ls /linux", timeout=60)
        assert "is gone" in out and "no Linux root" in out, f"linuxroot -d: {out!r}"
        out, rc = w.run("linux /bin/ls /", timeout=30)
        assert rc != 0, f"without a root there is no /bin/ls of the Linux kind (the native one is not a Linux program): {rc} {out!r}"


def make_archive(gz):
    """A tiny archive with the kinds of entries linuxroot must handle: directories with modes, files, a symbolic link, a hard link, a device node, a long name."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz" if gz else "w", format=tarfile.GNU_FORMAT) as tf:
        def add(name, type_, data=b"", mode=0o644, link=""):
            ti = tarfile.TarInfo(name)
            ti.type, ti.mode, ti.mtime, ti.size, ti.linkname = type_, mode, 1767225600, len(data), link
            ti.uid = ti.gid = 0
            tf.addfile(ti, io.BytesIO(data) if data else None)
        add("etc/", tarfile.DIRTYPE, mode=0o755)
        add("etc/greeting", tarfile.REGTYPE, b"hello from the archive\n", 0o640)
        add("bin/", tarfile.DIRTYPE, mode=0o755)
        add("bin/tool", tarfile.REGTYPE, b"#!/bin/sh\necho tool\n", 0o755)
        add("bin/tool-link", tarfile.SYMTYPE, link="tool")
        add("bin/tool-hard", tarfile.LNKTYPE, link="bin/tool")
        add("dev/", tarfile.DIRTYPE, mode=0o755)
        add("dev/zero", tarfile.CHRTYPE, mode=0o666)
        add("lib/", tarfile.DIRTYPE, mode=0o755)
        add("lib/" + "d" * 90 + "/" + "f" * 90, tarfile.REGTYPE, b"a long name\n", 0o600)
        add("tmp/", tarfile.DIRTYPE, mode=0o1777)
        add("etc/resolv.conf", tarfile.REGTYPE, b"nameserver 1.2.3.4\n", 0o644)
    return buf.getvalue()


@test
def linux_dyn_linuxroot_unpacks():
    """linuxroot unpacks a plain and a gzip-compressed tar into a tmpfs on /linux: modes, symbolic links, hard links and long names come out right, a device node is skipped
    with a line, resolv.conf is replaced by the machine's own, and `linuxroot -d` gives the memory back"""
    INPUTS.mkdir(parents=True, exist_ok=True)
    (INPUTS / "lnxtest-plain.tar").write_bytes(make_archive(False))
    (INPUTS / "lnxtest-gz.tar.gz").write_bytes(make_archive(True))
    long_name = "/linux/lib/" + "d" * 90 + "/" + "f" * 90
    with Qemu(cmdline="text user=root", name="t-linux-unpack", net="virtio-net-pci") as vm, Inputs() as srv:
        vm.expect(ROOT_PROMPT, timeout=300)
        for n in ("lnxtest-plain.tar", "lnxtest-gz.tar.gz"):
            fetch_root(vm, srv.port, n)
        out, rc = root_run(vm, "linuxroot; echo; ls -d /linux 2>&1")
        assert rc != 0 or "not mounted" in out, out
        for n in ("lnxtest-plain.tar", "lnxtest-gz.tar.gz"):
            out, rc = root_run(vm, f"linuxroot /tmp/{n} 2>&1", timeout=60)
            assert rc == 0 and "unpacked into /linux" in out, f"{n}: {rc} {out!r}"
            assert "skipped dev/zero" in out, f"{n}: the device node should be skipped with a line: {out!r}"
            out, rc = root_run(vm, "stat /linux/etc/greeting /linux/bin/tool /linux/tmp; cat /linux/etc/greeting; readlink /linux/bin/tool-link")
            assert rc == 0, out
            assert "(0640/-rw-r-----)" in out and "(0755/-rwxr-xr-x)" in out and "(1777/drwxrwxrwt)" in out, f"{n}: modes are kept: {out}"
            assert "hello from the archive" in out and out.rstrip().endswith("tool"), f"{n}: {out}"
            out, rc = root_run(vm, f"cat /linux/bin/tool-hard; stat {long_name}")
            assert rc == 0 and "echo tool" in out and "Size: 12" in out, f"{n}: hard link and long name: {rc} {out!r}"
            out, rc = root_run(vm, "ls /linux/dev; cat /linux/etc/resolv.conf")
            real, _ = root_run(vm, "cat /etc/resolv.conf")
            assert "zero" not in out, f"{n}: the device node must not be made: {out!r}"
            assert out.strip().endswith(real.strip()) and "1.2.3.4" not in out, f"{n}: resolv.conf comes from the machine: {out!r} vs {real!r}"
            out, rc = root_run(vm, f"linuxroot /tmp/{n} 2>&1")
            assert rc != 0 and "already holds" in out, f"{n}: {out!r}"
            out, rc = root_run(vm, "su reimu -c 'linuxroot -d' 2>&1")
            assert rc != 0 and "root" in out, f"{n}: only root may change the Linux root: {out!r}"
            out, rc = root_run(vm, "linuxroot -d; linuxroot; ls -d /linux/etc 2>&1")
            assert "is gone" in out and "not mounted" in out and "No such file" in out, f"{n}: -d: {out!r}"
        out, rc = root_run(vm, "linuxroot /tmp/missing.tar 2>&1")
        assert rc != 0 and "missing.tar" in out, out
        out, rc = root_run(vm, "ls -d /linux")
        assert rc == 0, out
