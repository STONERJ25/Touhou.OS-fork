"""The Linux personality (docs/COMPAT.md): static x86-64 Linux programs run under TouhouOS and behave like they do on a real Linux.

The test programs are freestanding C in user/linuxtest (no C library, only system calls). What a real Linux prints for them is stored in tests/linux/golden/, made
by tools/linux-oracle.py in WSL, and compared here line by line.

    python tools/qemu-slot.py -- python tests/run_all.py linux
"""
import difflib
import functools
import http.server
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu, BUILD, scaled

TESTS = "/ScarletDevilMansion/share/linux-tests"
GOLDEN = Path(__file__).resolve().parent / "linux" / "golden"
ORACLE = Path(__file__).resolve().parent.parent / "tools" / "linux-oracle.py"
INPUTS = BUILD / "linux-inputs"                       # statically linked test programs, built from tests/linux/src; never committed, never in an image
TRACE = " linux.trace" if os.environ.get("LINUX_TRACE") else ""        # LINUX_TRACE=1 python tests/run_all.py linux_static logs every call to the serial console


def build_inputs():
    """Builds the statically linked programs (tests/linux/src) with gcc on a Linux: inside WSL on Windows, directly elsewhere (tools/linux-oracle.py --inputs)."""
    if sys.platform == "win32":
        p = ORACLE.resolve()
        path = "/mnt/" + p.drive[0].lower() + "/" + "/".join(p.parts[1:])         # C:\Users\x\y -> /mnt/c/Users/x/y (WSL's default mount point)
        cmd = ["wsl.exe", "-e", "python3", path, "--inputs"]
    else:
        cmd = [sys.executable, str(ORACLE), "--inputs"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, f"building the test programs failed:\n{r.stdout}\n{r.stderr}"


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class Inputs:
    """Serves build/linux-inputs to the guest, which sees the host as 10.0.2.2."""

    def __enter__(self):
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(_Quiet, directory=str(INPUTS)))
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self.srv.shutdown()
        self.srv.server_close()


def fetch(vm, port, name):
    """Downloads one test program into /tmp of the guest (the network comes up a little after the prompt, so it retries) and makes it executable."""
    deadline = time.time() + scaled(180)
    while True:
        out, rc = test_shell.run(vm, f"fetch -q -O /tmp/{name} http://10.0.2.2:{port}/{name}", timeout=120)
        if rc == 0:
            break
        assert time.time() < deadline, f"could not download {name}: {out!r}"
        time.sleep(3)
    test_shell.run(vm, f"chmod +x /tmp/{name}")


def run(vm, cmd, timeout=90):
    out, rc = test_shell.run(vm, cmd, timeout=timeout)
    return out.replace("\r", ""), rc


def compare_with_linux(name, out, rc, kernel_tags=()):
    """The guest printed what the stored output of a real Linux says, and ended with the same status. Lines of the kernel log that start with one of kernel_tags
    ("[Eiki]" announces a process that a signal killed) share the console with the program's output; they are set aside and returned."""
    want = (GOLDEN / f"{name}.txt").read_text(encoding="utf-8")
    kernel = []
    if kernel_tags:
        kernel = [l for l in out.splitlines() if l.startswith(tuple(kernel_tags))]
        out = "".join(l + "\n" for l in out.splitlines() if not l.startswith(tuple(kernel_tags)))
    got = out + f"exit status: {rc}\n"
    if got != want:
        diff = "\n".join(difflib.unified_diff(want.splitlines(), got.splitlines(), "real Linux", "TouhouOS", lineterm="", n=0))
        raise AssertionError(f"{name} differs from a real Linux:\n{diff}")
    return kernel


@test
def linux_hello_and_exit_status():
    """linux PROGRAM runs a program with no C library: write and exit_group work, and the status reaches the shell (exit_group(42) is 42)"""
    with Qemu(cmdline="text", name="t-linux-hello") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"linux {TESTS}/lnx_hello")
        compare_with_linux("lnx_hello", out, rc)
        out, rc = run(vm, f"linux {TESTS}/lnx_exit")
        assert rc == 42, f"exit status {rc}, wanted 42 ({out!r})"
        out, rc = run(vm, f"linux {TESTS}/lnx_exit 3")
        assert rc == 3, f"exit status {rc}, wanted 3 ({out!r})"
        out, rc = run(vm, "linux /no/such/program")
        assert rc == 127 and "cannot run" in out, f"{rc} {out!r}"
        out, rc = run(vm, "linux")
        assert rc == 2 and "usage" in out, f"{rc} {out!r}"


@test
def linux_initial_stack_matches_linux():
    """The stack a Linux program starts with: aligned, argc/argv/envp, and the auxiliary vector a static C library reads (the same lines as on a real Linux)"""
    with Qemu(cmdline="text", name="t-linux-args") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"FOO=bar linux {TESTS}/lnx_args alpha beta")
        compare_with_linux("lnx_args", out, rc)


@test
def linux_probe_matches_linux():
    """Fifty-odd system calls, many of them wrong on purpose (errno values, brk, mmap, arch_prctl, stat, pipes, poll, clocks, fork and wait4): every result is the one
    a real Linux gives"""
    with Qemu(cmdline="text", name="t-linux-probe") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"linux {TESTS}/lnx_probe", timeout=180)
        compare_with_linux("lnx_probe", out, rc)


@test
def linux_exec_keeps_the_personality():
    """execve from a Linux program starts a Linux program (the personality is kept), and the errors of execve are the ones execve(2) names"""
    with Qemu(cmdline="text", name="t-linux-exec") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"linux {TESTS}/lnx_exec")
        want = ("execve of a missing file: -2\nexecve of a directory: -13\nexecve with a bad path pointer: -14\n"
                "starting the next program\nhello from a Linux program\n")
        assert out == want and rc == 0, f"status {rc}\n{out}"


@test
def linux_missing_calls_are_named_once():
    """A call the personality lacks answers ENOSYS and the kernel log names it once ("not implemented"); a number that is no system call at all is answered
    with ENOSYS and not logged"""
    with Qemu(cmdline="text", name="t-linux-enosys") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"linux {TESTS}/lnx_enosys")
        lines = [l for l in out.splitlines() if not l.startswith("[Outside World]")]       # the log line of the first miss is on the console too
        assert lines == ["inotify_init: -38", "inotify_init again: -38", "a number that is not a system call: -38"] and rc == 0, f"{rc} {out!r}"
        out, _ = run(vm, "dmesg | grep -c 'Linux system call 253 (inotify_init) is not implemented'")
        assert out.strip() == "1", f"the log named inotify_init {out.strip()!r} times"
        out, _ = run(vm, "dmesg | grep -c 'Linux system call 5000'")
        assert out.strip() == "0", f"a number beyond the table must not be logged: {out!r}"


@test
def linux_trace_option_logs_calls():
    """linux.trace on the kernel command line logs every Linux system call with its arguments and result"""
    with Qemu(cmdline="text linux.trace", name="t-linux-trace") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, f"linux {TESTS}/lnx_hello")
        assert rc == 0 and "hello from a Linux program" in out, f"{rc} {out!r}"
        out, _ = run(vm, "dmesg | grep -c 'write(0x1, .*) = 27'")
        assert out.strip() == "1", f"the traced write: {out!r}"
        out, _ = run(vm, "dmesg | grep -c 'exit_group(0x0'")
        assert int(out.strip() or "0") >= 1, f"the traced exit_group: {out!r}"


# name -> (the command line in the guest, its time limit in seconds)
STATIC = {
    "glibc_hello": ("linux /tmp/glibc_hello", 90),
    "glibc_libc": ("FOO=bar linux /tmp/glibc_libc alpha beta", 180),
    "glibc_dir": ("linux /tmp/glibc_dir", 180),
    "glibc_sys": ("linux /tmp/glibc_sys", 180),
    "glibc_signals": ("linux /tmp/glibc_signals", 300),
    "glibc_threads": ("linux /tmp/glibc_threads", 300),
}


# the kernel announces a process that a signal killed, on the console the program writes to
KERNEL_TAGS = {"glibc_signals": ("[Eiki]",)}
EXPECTED_KERNEL_LINES = {"glibc_signals": ("crossed the Boundary at 0x10", "was exorcised by signal 15", "was exorcised by signal 6")}


def run_static(names, vm_name):
    """Downloads the programs into a guest with a network card, runs each and compares its output with the stored output of a real Linux. LINUX_ONLY=a,b limits
    the programs (for debugging one of them); LINUX_TRACE=1 logs every call."""
    only = os.environ.get("LINUX_ONLY")
    names = [n for n in names if not only or n in only.split(",")]
    if not names:
        return
    build_inputs()
    failures = []
    with Inputs() as srv, Qemu(cmdline="text" + TRACE, name=vm_name, net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        for n in names:
            fetch(vm, srv.port, n)
        for n in names:
            cmd, limit = STATIC[n]
            limit = int(os.environ.get("LINUX_LIMIT", limit))
            out, rc = run(vm, cmd, timeout=limit)
            try:
                kernel = compare_with_linux(n, out, rc, KERNEL_TAGS.get(n, ()))
                for what in EXPECTED_KERNEL_LINES.get(n, ()):
                    assert any(what in l for l in kernel), f"{n}: the kernel log lacks {what!r}; it had {kernel}"
            except AssertionError as e:
                failures.append(str(e))
    assert not failures, "\n\n".join(failures)


@test
def linux_static_glibc_programs():
    """Programs linked statically with a real C library (glibc 2.x) run: its start-up (brk, arch_prctl, set_tid_address, prlimit64, TLS setup), then printf, the
    heap (brk and mmap), sorting, floating point, files and directories (openat, getdents64, stat, rename), errno, fork and wait, pipes, clocks, statx, sysinfo,
    prctl, atexit and the exit status: the same output as on a real Linux"""
    run_static(["glibc_hello", "glibc_libc", "glibc_dir", "glibc_sys"], "t-linux-glibc")


@test
def linux_static_glibc_signals():
    """Signals through glibc: handlers with and without siginfo, blocking and pending, SIG_IGN, SA_RESETHAND, sigsuspend, alarm and pause, a SIGSEGV handler that
    leaves with siglongjmp (si_addr, si_code), the alternate stack, floating-point state kept across a handler, SA_RESTART, the default actions seen by wait,
    SIGCHLD: the same output as on a real Linux"""
    run_static(["glibc_signals"], "t-linux-signals")


@test
def linux_static_glibc_threads():
    """POSIX threads through glibc (clone with CLONE_THREAD, futex, thread-local storage, tgkill): mutexes under contention, condition variables with and without
    a timeout, semaphores, join values, pthread_exit, thread ids, detached threads, sixteen threads at once, a signal aimed at one thread: the same output as on a
    real Linux"""
    run_static(["glibc_threads"], "t-linux-threads")
