"""L3 of docs/COMPAT.md: Python from the Linux root. The real python3 of the Ubuntu in WSL (3.12.3) and its standard library are unpacked into the guest by `linuxroot`
(tools/make-linux-root.py --python), and a corpus of scripts (tests/linux/py, one per area of what a real program uses: arithmetic, text, hashing, compression, ctypes, sqlite3,
the os module, subprocess, threads, signals, sockets, select and epoll, asyncio, mmap, multiprocessing) runs in it. What the same scripts print on a real Linux, run by
tools/linux-oracle.py in WSL with a fixed environment, is stored in tests/linux/golden/py_NAME.txt and compared line by line.

Nothing of the Python side is committed: the interpreter, its library and the archive are built under build/linux-inputs at run time, and without WSL (or a Linux with python3 3.12.3
and gcc 13.3: the versions the stored outputs belong to) these tests print SKIP and pass.

    python tools/qemu-slot.py -- python tests/run_all.py linux_py
    LINUX_ONLY=arith,strfmt python tests/run_all.py linux_py_language       limits the scripts; LINUX_TRACE=1 logs every call; LINUX_LIMIT=300 sets the time limit

The guests need more memory than the others (1.5 GB): the root unpacks into a tmpfs that may use half of it (it is 40 MB of files), every Python process holds about 20 MB, a fork
copies all of it (there is no copy on write), and the multiprocessing and mmap scripts keep a dozen processes alive at once.
"""
import contextlib
import difflib
import importlib.util
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import test, Qemu
from case_linux import Inputs, build_inputs, INPUTS, GOLDEN, TRACE
from case_linux_dyn import root_run, fetch_root, ROOT_PROMPT, skip_reason, TOOLS

def oracle():
    """tools/linux-oracle.py as a module (its file name has a hyphen): the versions of Python and gcc the stored outputs belong to, and the table of scripts."""
    spec = importlib.util.spec_from_file_location("linux_oracle", TOOLS / "linux-oracle.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def host_says(*argv):
    """The first line that a command prints on the machine the roots are made on (inside WSL on Windows), or "" when it cannot be run."""
    cmd = list(argv)
    if sys.platform == "win32":
        wsl = shutil.which("wsl.exe")
        if not wsl:
            return ""
        cmd = [wsl, "-e", *cmd]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return ""
    return r.stdout.splitlines()[0].strip() if r.returncode == 0 and r.stdout.strip() else ""


def py_skip_reason():
    """Why the Python tests cannot run here (None when they can): no WSL or gcc (skip_reason), or a python3 that is not the one whose output is stored."""
    why = skip_reason()
    if why:
        return why
    want, have = oracle().PY_VERSION, host_says("python3", "--version")
    return None if have == want else f"the python3 of this machine is {have!r}; the stored outputs of the scripts and the root made from it are those of {want!r}"


def gcc_skip_reason():
    """The same for the compiler tests: the gcc of the machine must be the one whose output is stored."""
    why = skip_reason()
    if why:
        return why
    want, have = oracle().GCC_VERSION, host_says("gcc", "--version")
    return None if have == want else f"the gcc of this machine is {have!r}; the stored outputs of the compiler tests are those of {want!r}"


# the Linux roots these tests use: kind -> (archive in build/linux-inputs, flags of tools/make-linux-root.py)
ROOTS = {"python": ("linux-root-python.tar.gz", ["--python"]), "gcc": ("linux-root-gcc.tar.gz", ["--gcc"])}
MEM = "1536M"
# what the oracle sets (tools/linux-oracle.py PY_ENV) and the guest sets the same way: the script sees exactly these variables
ENV = "PATH=/usr/bin:/bin LC_ALL=C.UTF-8 PYTHONHASHSEED=0 PYTHONUNBUFFERED=1 HOME=/tmp TZ=UTC"
KERNEL_TAGS = ("[Outside World]", "[Eiki]")     # lines of the kernel log that share the console with the script's output are set aside

# the scripts by area, and their time limit in the guest (seconds, before TOUHOU_TIMEOUT_SCALE)
LANGUAGE = {"hello": 180, "arith": 180, "strfmt": 180, "jsonre": 180, "dates": 180, "rng": 180, "hashing": 240, "compress": 300, "unittests": 180, "ffi": 180, "sqldb": 240}
SYSTEM = {"osops": 240, "oscalls": 240, "subproc": 300, "signals": 300, "clock": 240, "mmaps": 240}
CONCURRENCY = {"threads": 240, "procs": 360}
NETWORK = {"sockets": 300, "polling": 240, "aio": 300}


def build_root(kind):
    """build/linux-inputs/<archive of that kind> from the Ubuntu in WSL (tools/make-linux-root.py with its flags and --gzip), unless it is newer than the script."""
    archive, flags = ROOTS[kind]
    gz = INPUTS / archive
    script = TOOLS / "make-linux-root.py"
    if gz.exists() and gz.stat().st_mtime > script.stat().st_mtime:
        return gz
    r = subprocess.run([sys.executable, str(script), *flags, "--gzip"], capture_output=True, text=True)
    assert r.returncode == 0 and gz.exists(), f"building the Linux root ({' '.join(flags)}) failed:\n{r.stdout}\n{r.stderr}"
    return gz


class PyWorld:
    """A guest booted as root with a network card, the Linux root with Python unpacked into /linux, and the scripts downloaded into /tmp/py.

        with PyWorld("t-linux-py-x", ["arith"]) as w:
            out, rc = w.run("linux /usr/bin/python3 -c 'print(1 + 1)'")
            text = w.script("arith")                       # what `python3 arith.py` printed, with "exit status: N" at the end
    """

    def __init__(self, name, scripts=(), root="python", mem=MEM, cmdline="text user=root", files=()):
        self.name, self.scripts, self.root, self.mem, self.cmdline, self.files = name, list(scripts), root, mem, cmdline, list(files)

    def __enter__(self):
        build_inputs()
        build_root(self.root)
        archive = ROOTS[self.root][0]
        self.stack = contextlib.ExitStack()
        try:
            self.srv = self.stack.enter_context(Inputs())
            self.vm = self.stack.enter_context(Qemu(cmdline=self.cmdline + TRACE, name=self.name, net="virtio-net-pci", mem=self.mem))
            self.vm.expect(ROOT_PROMPT, timeout=300)
            fetch_root(self.vm, self.srv.port, archive)
            t0 = time.time()
            out, rc = root_run(self.vm, f"linuxroot /tmp/{archive}", timeout=600)
            assert rc == 0, f"linuxroot failed ({rc}): {out}"
            self.unpack_seconds = time.time() - t0
            root_run(self.vm, f"rm /tmp/{archive}")                             # the archive in RAM is not needed any more
            root_run(self.vm, "mkdir /tmp/py /tmp/cc")
            for n in self.scripts:
                fetch_root(self.vm, self.srv.port, f"py/{n}.py")
            for n in self.files:                                                # other inputs: "cc/cc_hello.c" goes to /tmp/cc/cc_hello.c
                fetch_root(self.vm, self.srv.port, n)
        except BaseException:
            self.stack.close()
            raise
        return self

    def __exit__(self, *exc):
        self.stack.close()
        return False

    def run(self, cmd, timeout=90):
        return root_run(self.vm, cmd, timeout=int(os.environ.get("LINUX_LIMIT", timeout)))

    def script(self, name, timeout=240, args=""):
        """Runs /tmp/py/NAME.py as the oracle does: the fixed environment, an empty working directory, standard input from /dev/null, standard error mixed into standard output, and the
        exit status appended as a line. The wrapper is the dash of the Linux root, because the shell of the native system gives a pipeline's last status and runs the first external
        command of a subshell in place. Returns the text without the kernel's own lines."""
        wk = "/tmp/wk"
        cmd = (f"linux /bin/sh -c 'rm -rf {wk}; mkdir {wk}; cd {wk}; /usr/bin/env -i {ENV} /usr/bin/python3 /tmp/py/{name}.py {args} 2>&1 </dev/null; echo \"exit status: $?\"' | cat")
        out, _ = self.run(cmd, timeout=timeout)
        return "".join(l + "\n" for l in out.splitlines() if not l.startswith(KERNEL_TAGS))


def selected(names):
    only = os.environ.get("LINUX_ONLY")
    return [n for n in names if not only or n in only.split(",")]


def compare_scripts(w, limits, failures):
    """Runs each script of `limits` (name -> seconds) in the world and compares what it printed with the stored output of a real Linux."""
    for n in selected(limits):
        want = (GOLDEN / f"py_{n}.txt").read_text(encoding="utf-8")
        t0 = time.time()
        got = w.script(n, timeout=limits[n])
        seconds = time.time() - t0
        print(f"     (py_{n}: {seconds:.1f} s)")
        if got != want:
            diff = "\n".join(difflib.unified_diff(want.splitlines(), got.splitlines(), "real Linux", "TouhouOS", lineterm="", n=1))
            failures.append(f"py_{n} differs from a real Linux:\n{diff[:3000]}")


def run_area(vm_name, limits):
    if py_skip_reason():
        print("SKIP:", py_skip_reason())
        return
    names = selected(limits)
    if not names:
        return
    unknown = [n for n in names if n not in oracle().PY_PROGRAMS]
    assert not unknown, f"scripts that tools/linux-oracle.py does not know (PY_PROGRAMS): {unknown}"
    failures = []
    with PyWorld(vm_name, names) as w:
        compare_scripts(w, limits, failures)
    assert not failures, "\n\n".join(failures)


@test
def linux_py_first_program():
    """python3 -c 'print(1+1)' from the Linux root: the interpreter (a position-independent executable with libm, libz, libexpat and libc) starts, finds its library under /usr/lib/python3.12,
    imports its modules and prints; the exit status reaches the shell; a script file, an exception, sys.exit and a missing file behave as they do on a real Linux"""
    if py_skip_reason():
        print("SKIP:", py_skip_reason())
        return
    with PyWorld("t-linux-py-first", []) as w:
        out, rc = w.run("linux /usr/bin/python3 -c 'print(1+1)'", timeout=180)
        assert rc == 0 and out.strip().splitlines()[-1] == "2", f"print(1+1): {rc} {out!r}"
        print(f"     (the root unpacked in {w.unpack_seconds:.1f} s; python3 -c 'print(1+1)' first run: see the corpus for timings)")
        out, rc = w.run("linux /usr/bin/python3 -c 'import sys; sys.exit(7)'", timeout=120)
        assert rc == 7, f"sys.exit(7): {rc} {out!r}"
        out, rc = w.run("linux /usr/bin/python3 -c '1/0' 2>&1", timeout=120)
        assert rc == 1 and "ZeroDivisionError: division by zero" in out, f"an exception: {rc} {out!r}"
        out, rc = w.run("linux /usr/bin/python3 /tmp/nothing.py 2>&1", timeout=120)
        assert rc == 2 and "No such file or directory" in out, f"a missing script: {rc} {out!r}"
        out, rc = w.run("echo 'print(sum(range(101)))' > /tmp/sum.py; linux /usr/bin/python3 /tmp/sum.py", timeout=120)
        assert rc == 0 and out.strip().splitlines()[-1] == "5050", f"a script file: {rc} {out!r}"
        out, rc = w.run("linux /bin/sh -c 'echo import_this_is_a_test | /usr/bin/python3 -c \"import sys; print(sys.stdin.read().upper(), end=chr(33))\"'", timeout=120)
        assert rc == 0 and "IMPORT_THIS_IS_A_TEST" in out and out.strip().endswith("!"), f"standard input from a pipe: {rc} {out!r}"
        # a user without privileges: the files of the root are readable, the library cannot be compiled into the tree (every import is compiled again), the names come from the root's /etc
        out, rc = w.run("su reimu -c 'linux /usr/bin/python3 -c \"import os, pwd, json; print(os.getuid(), pwd.getpwuid(os.getuid()).pw_name, json.dumps([1, 2]))\"' 2>&1", timeout=300)
        assert rc == 0 and "1000 reimu [1, 2]" in out, f"python3 as an unprivileged user: {rc} {out!r}"
        # since 2026-11-17 execve takes the personality from the file (docs/COMPAT.md, "The personality is chosen by the file"): the interpreter, a position-independent executable with a
        # PT_INTERP, started by its path from the native shell with no `linux` in front, is a Linux program (the root is /linux in the real tree)
        out, rc = w.run("/linux/usr/bin/python3 -c 'import sys; print(6 * 7, sys.version_info[:2])' 2>&1", timeout=180)
        assert rc == 0 and out.strip().splitlines()[-1] == "42 (3, 12)", f"python3 started by its path from the native shell: {rc} {out!r}"


@test
def linux_py_language():
    """The language and its library on the interpreter of the Linux root, each script against a real Linux: the first script, integers of any size and floating point, strings
    and unicode, json and regular expressions, dates and times, random numbers with a seed, hashing (md5 to blake2, hmac, pbkdf2, scrypt), zlib, gzip, bz2, lzma, zip and tar
    archives, unittest, ctypes with callbacks into Python, and sqlite3 with database files"""
    run_area("t-linux-py-language", LANGUAGE)


@test
def linux_py_system():
    """The os module and the processes around it, each script against a real Linux: files, directories and descriptors with their errno values, fork, exec, wait and the exit statuses,
    the less common calls (mkfifo, fallocate, preadv, copy_file_range, memfd_create, scheduling, locks, terminals), subprocess with pipes and time-outs, signals and timers,
    sleeping and the clocks, and mmap, including memory shared with a forked child"""
    run_area("t-linux-py-system", SYSTEM)


@test
def linux_py_concurrency():
    """threading, queue and concurrent.futures (locks, conditions, events, barriers, thread pools, forty threads at once) and multiprocessing with the fork start method (processes,
    queues, pipes, pools, shared values and arrays, semaphores from sem_open, shared_memory, a manager process), each script against a real Linux"""
    run_area("t-linux-py-concurrency", CONCURRENCY)


@test
def linux_py_network():
    """Sockets over the loopback interface (TCP with a server thread and a 1 MB echo, UDP, UNIX-domain sockets and descriptor passing, getaddrinfo, socketserver, an HTTP server
    and urllib), select, poll and epoll with the selectors module, and asyncio on the epoll loop (streams, servers, subprocesses, signal handlers, threads), each script against a
    real Linux"""
    run_area("t-linux-py-network", NETWORK)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the C compiler

GCC_CASES = ["gcc_hello_object", "gcc_calc_object", "gcc_calc_assembly", "gcc_hello_program", "gcc_calc_program"]
GCC_FILES = ["cc/cc_hello.c", "cc/cc_calc.c"]


def replay_gcc_case(w, name, limit):
    """Runs the command the stored text of a real Linux starts with (`$ gcc ARGS`) in the guest, in a fresh directory with the sources, and describes what came out the way the oracle
    does (tools/linux-oracle.py, run_gcc_case): the command, its status and messages, the size and sha256 of the file it made, and for a program its run. Returns the text."""
    want = (GOLDEN / f"{name}.txt").read_text(encoding="utf-8").splitlines()
    assert want[0].startswith("$ gcc "), f"{name}: the stored text does not start with the command: {want[0]!r}"
    args = want[0][len("$ gcc "):]
    produced = next((l.split(":")[0] for l in want if " bytes, sha256 " in l), None)
    run_line = next((l[2:] for l in want if l.startswith("$ ./")), None)
    w.run("rm -rf /tmp/cc/w; mkdir /tmp/cc/w; cp /tmp/cc/cc_hello.c /tmp/cc/cc_calc.c /tmp/cc/w", timeout=60)
    out, rc = w.run(f"cd /tmp/cc/w; linux /usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C /usr/bin/gcc {args} > log 2>&1; echo status: $?; cat log", timeout=limit)
    lines = [f"$ gcc {args}"] + [l for l in out.splitlines() if not l.startswith(KERNEL_TAGS)]
    if produced:
        size, rc = w.run(f"wc -c < /tmp/cc/w/{produced}", timeout=60)
        digest, rc2 = w.run(f"sha256sum /tmp/cc/w/{produced}", timeout=120)
        if rc == 0 and rc2 == 0 and digest.split():
            lines.append(f"{produced}: {size.strip()} bytes, sha256 {digest.split()[0]}")
        if run_line:
            w.run(f"chmod +x /tmp/cc/w/{produced}", timeout=30)
            out, rc = w.run(f"cd /tmp/cc/w; linux {run_line} 2>&1; echo exit status: $?", timeout=limit)
            lines.append(f"$ {run_line}")
            lines += [l for l in out.splitlines() if not l.startswith(KERNEL_TAGS)]
    return "\n".join(lines) + "\n"


@test
def linux_py_gcc_compiles():
    """gcc from the Linux root (the driver, cc1, the assembler and the linker of the Ubuntu in WSL, the headers of /usr/include, libgcc and the start-up files): `gcc -c` of two C programs
    gives an object file of exactly the bytes the same gcc gives on a real Linux (sha256), -S gives the same assembly text, and `gcc file.c -o program` links a dynamic program that
    runs and prints what the one built there prints"""
    if gcc_skip_reason():
        print("SKIP:", gcc_skip_reason())
        return
    failures = []
    with PyWorld("t-linux-py-gcc", [], root="gcc", mem="2G", files=GCC_FILES) as w:
        print(f"     (the root with gcc unpacked in {w.unpack_seconds:.1f} s)")
        out, rc = w.run("linux /usr/bin/gcc --version 2>&1 | head -1", timeout=300)
        assert "gcc (Ubuntu" in out, f"gcc --version: {rc} {out!r}"
        for n in selected(GCC_CASES):
            want = (GOLDEN / f"{n}.txt").read_text(encoding="utf-8")
            t0 = time.time()
            got = replay_gcc_case(w, n, 900)
            print(f"     ({n}: {time.time() - t0:.1f} s)")
            if got != want:
                diff = "\n".join(difflib.unified_diff(want.splitlines(), got.splitlines(), "real Linux", "TouhouOS", lineterm="", n=1))
                failures.append(f"{n} differs from a real Linux:\n{diff[:2500]}")
    assert not failures, "\n\n".join(failures)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# the interactive interpreter

@test
def linux_py_interactive():
    """python3 typed to over the console, as a person uses it: the prompt, an expression's value, a block that takes several lines, a traceback, the terminal's size and kind,
    and exit(): readline switches the terminal to its own settings with Linux's struct termios (translated to the native one by the Linux personality) and echoes what is typed once"""
    if py_skip_reason():
        print("SKIP:", py_skip_reason())
        return
    with PyWorld("t-linux-py-repl", []) as w:
        vm = w.vm
        vm.send("linux /usr/bin/python3\n")
        vm.expect(r">>> ", timeout=300)
        mark = len(vm.text())
        vm.send("print(6 * 7)\n")
        vm.expect(r"\n42\r?\n", timeout=120)
        vm.expect(r">>> ", timeout=60)
        typed = vm.text()[mark:]
        assert typed.count("print(6 * 7)") == 1, f"the line should be echoed once (readline's echo), not twice:\n{typed!r}"
        vm.send("sum(range(101))\n")
        vm.expect(r"\n5050\r?\n", timeout=60)
        vm.expect(r">>> ", timeout=60)
        vm.send("for i in range(3):\n")
        vm.expect(r"\.\.\. ", timeout=60)
        vm.send("    print('row', i)\n")
        vm.expect(r"\.\.\. ", timeout=60)
        vm.send("\n")
        vm.expect(r"\nrow 0\r?\nrow 1\r?\nrow 2\r?\n", timeout=60)
        vm.expect(r">>> ", timeout=60)
        vm.send("import os, sys; print(sys.stdin.isatty(), sys.stdout.isatty(), os.get_terminal_size() > (0, 0))\n")
        vm.expect(r"\nTrue True True\r?\n", timeout=60)
        vm.expect(r">>> ", timeout=60)
        vm.send("1/0\n")
        vm.expect(r"ZeroDivisionError: division by zero", timeout=60)
        vm.expect(r">>> ", timeout=60)
        vm.send("abcX\x7f\x7f\x7f\x7f\n")                           # typed and rubbed out again: readline's line editing, then a syntax-error-free empty line
        vm.expect(r">>> ", timeout=60)
        vm.send("exit()\n")
        vm.expect(ROOT_PROMPT, timeout=120)
