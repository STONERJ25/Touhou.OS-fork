"""Windows console programs under winrun (docs/WINDOWS.md): PE32+ images built on the development PC with clang and lld-link (tools/make-winprogs.py) are
downloaded into the guest and run with `winrun PROGRAM.EXE`. One boot runs them all (the QEMU slots are shared).

    python tools/qemu-slot.py -- python tests/run_all.py windows
"""
import functools
import http.server
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu, BUILD, scaled

ROOT = Path(__file__).resolve().parent.parent
PROGS = BUILD / "winprogs"
NAMES = ["hello", "exitcode", "args", "heap", "unused_import", "called_import", "reloc", "reloc_low", "otherdll", "files", "damaged",
         "fizzbuzz", "sort", "strings", "deep", "checksum", "teb", "fsops"]


def build_programs():
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "make-winprogs.py")], capture_output=True, text=True)
    assert r.returncode == 0, f"building the Windows test programs failed:\n{r.stdout}\n{r.stderr}"
    # a damaged copy: hello.exe cut off in the middle of its code section (the loader must refuse it, not crash)
    data = (PROGS / "hello.exe").read_bytes()
    (PROGS / "damaged.exe").write_bytes(data[:len(data) - 300])


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class Serve:
    def __enter__(self):
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(_Quiet, directory=str(PROGS)))
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self.srv.shutdown()
        self.srv.server_close()


def run(vm, cmd, timeout=90):
    out, rc = test_shell.run(vm, cmd, timeout=timeout)
    return out.replace("\r", ""), rc


def fetch_all(vm, port):
    """Downloads every program into /tmp/win (the network comes up a little after the prompt, so the first one retries)."""
    run(vm, "mkdir -p /tmp/win")
    deadline = time.time() + scaled(180)
    for n in NAMES:
        while True:
            out, rc = run(vm, f"fetch -q -O /tmp/win/{n}.exe http://10.0.2.2:{port}/{n}.exe", timeout=120)
            if rc == 0:
                break
            assert time.time() < deadline, f"could not download {n}.exe: {out!r}"
            time.sleep(3)


@test
def windows_console_programs_run():
    """winrun: hello prints its line (status 0), ExitProcess(42) is 42, the command line is built the Windows way, the heap and VirtualAlloc work, an
    import that is never called is harmless and one that is called stops with its name and 127, a moved image is relocated, a DLL that is not built in and
    a damaged file are refused with 126"""
    build_programs()
    with Serve() as srv, Qemu(cmdline="text", name="t-windows", net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        fetch_all(vm, srv.port)
        out, rc = run(vm, "winrun /tmp/win/hello.exe")
        assert out.strip() == "Hello from a Windows program on TouhouOS!" and rc == 0, f"{rc} {out!r}"
        out, rc = run(vm, "winrun /tmp/win/exitcode.exe")
        assert rc == 42 and out.strip() == "", f"{rc} {out!r}"
        out, rc = run(vm, 'WINTEST=marisa winrun /tmp/win/args.exe one "two words" \'say "hi"\'')
        want = ('cmdline: [/tmp/win/args.exe one "two words" "say \\"hi\\""]\nwide: same\nmodule: ok\nWINTEST=marisa\nunset: 0 error 203\n')
        assert out == want and rc == 3, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/heap.exe", timeout=180)
        assert out.startswith("heap ok: 4096 operations") and rc == 0, f"{rc} {out!r}"
        out, rc = run(vm, "winrun /tmp/win/unused_import.exe")
        assert out.strip() == "unused import: fine" and rc == 0, f"{rc} {out!r}"
        out, rc = run(vm, "winrun /tmp/win/called_import.exe")
        assert out.splitlines() == ["before the call", "winrun: unimplemented KERNEL32.dll!WinrunNeverHasThis"] and rc == 127, f"{rc} {out!r}"
        out, rc = run(vm, "winrun -v /tmp/win/reloc.exe")
        assert "image at 0x140000000 (ImageBase 0x140000000" in out and "reloc table ok" in out and "consistent" in out and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "winrun -v /tmp/win/reloc_low.exe")
        assert "ImageBase 0x400000 not used" in out and "relocations applied" in out and "reloc table ok" in out and "consistent\n" in out and rc == 0, f"{rc}\n{out}"
        assert "image at 0x400000 " not in out, out
        out, rc = run(vm, "winrun /tmp/win/otherdll.exe")
        assert rc == 126 and "USER32.dll, which is not built in" in out and "must not" not in out, f"{rc} {out!r}"
        out, rc = run(vm, "winrun /tmp/win/damaged.exe")
        assert rc == 126 and "winrun: /tmp/win/damaged.exe:" in out, f"{rc} {out!r}"
        out, rc = run(vm, "winrun /tmp/win/files.exe /tmp/win/out.txt")
        want = ("create 1 error 0\nwrite 1 error 0\ntype 1 error 0\ncreate_new on an existing file 0 error 80\nopen_always 1 error 183\nsize 29 error 0\n"
                "read back 1 error 0\nread at the end 1 error 0\nclose 1 error 0\nclose again 0 error 6\nmissing 0 error 2\n")
        assert out == want and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "cat /tmp/win/out.txt")
        assert out.strip() == "written by a Windows program", out
        out, rc = run(vm, "winrun")
        assert rc == 2 and "usage" in out, f"{rc} {out!r}"
        out, rc = run(vm, "echo alive")
        assert out.strip() == "alive", out


@test
def windows_gs_base_per_thread():
    """SYS_gsbase (gap_set_gsbase): two threads keep their own gs: bases across context switches, a signal handler and a new thread see the base, fork
    inherits it, execve clears it (user/test/gstest.c)"""
    with Qemu(cmdline="text", name="t-windows-gs") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        out, rc = run(vm, "/usr/lib/kernel-tests/gstest", timeout=180)
        assert rc == 0 and out.rstrip().endswith("gstest: ok") and "FAILED" not in out, f"{rc}\n{out}"
        assert "execve clears the base: ok" in out, out


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


@test
def windows_c_runtime_programs():
    """W2: programs with main(argc, argv) on the mini C runtime (tests/win/crt) run under winrun with a TEB and a PEB behind gs: FizzBuzz and printf's
    formats, qsort over 20000 numbers, string functions and the argv quoting rules and atexit, a 5000-deep recursion and a 64 KiB frame through __chkstk,
    a file checksum, and the TEB/PEB fields a C runtime reads (the same programs were checked on a real Windows 11 first)"""
    build_programs()
    with Serve() as srv, Qemu(cmdline="text", name="t-windows-crt", net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        fetch_all(vm, srv.port)
        out, rc = run(vm, "winrun /tmp/win/fizzbuzz.exe")
        fizz = ["FizzBuzz" if i % 15 == 0 else "Fizz" if i % 3 == 0 else "Buzz" if i % 5 == 0 else str(i) for i in range(1, 16)]
        want = "\n".join(fizz) + ("\n[   42][42   ][00042][ff][FF][10][k][str][abc][   right][left    |]\n"
                                  "[-7][-123456789012345][99][4000000000][-5][+3][%][0xbeef][  z]\n[     007][+9    ][][0x1234]\n")
        assert out == want and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/sort.exe zeta alpha mid")
        assert out == "sorted 20000 numbers: min 172 max 16776842 checksum 86319cc60f012e96\nalpha mid zeta\n" and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/strings.exe one 'two words' 'say \"hi\"' 'back\\slash'")
        want = ("Reimu and Marisa (16)\nstrchr: Marisa, missing: NULL\nstrcmp: 1 1 0, strncmp: 0\nmemmove up: 0101234589\nmemmove down: 1234534589\n"
                "memcmp: 1\nsnprintf: [truncat] wanted 15\nargv[0] = [/tmp/win/strings.exe]\nargv[1] = [one]\nargv[2] = [two words]\nargv[3] = [say \"hi\"]\n"
                "argv[4] = [back\\slash]\natexit: second registered, first run\natexit: first registered, last run\n")
        assert out == want and rc == 7, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/deep.exe")
        assert out == "recursion: 5000\n64 KiB frame: 384\n" and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/checksum.exe /tmp/win/hello.exe")
        want = f"/tmp/win/hello.exe: {len((PROGS / 'hello.exe').read_bytes())} bytes, fnv1a64 {fnv1a64((PROGS / 'hello.exe').read_bytes()):016x}\n"
        assert out == want and rc == 0, f"{rc}\n{out}"
        out, rc = run(vm, "winrun /tmp/win/teb.exe")
        assert rc == 0 and "FAILED" not in out and out.count(": ok\n") == 15 and out.endswith("module: /tmp/win/teb.exe\n"), f"{rc}\n{out}"
        # W2 kernel32 by name: directories, attributes, file positions, the UTF-8 conversions; the expected text is what fsops.exe prints on Windows 11
        out, rc = run(vm, "cd /tmp/win && winrun fsops.exe fsdir")
        assert out == FSOPS and rc == 0, f"{rc}\n{out}"


@test
def windows_exec_runs_mz_files():
    """W3's first step: execve of a file that starts with MZ runs /bin/winrun with its path, like a #! script, so ./hello.exe works from the shell and its
    status and arguments arrive"""
    build_programs()
    with Serve() as srv, Qemu(cmdline="text", name="t-windows-exec", net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        run(vm, "mkdir -p /tmp/win")
        deadline = time.time() + scaled(180)
        for n in ("hello", "exitcode", "strings"):
            while True:
                out, rc = run(vm, f"fetch -q -O /tmp/win/{n}.exe http://10.0.2.2:{srv.port}/{n}.exe", timeout=120)
                if rc == 0:
                    break
                assert time.time() < deadline, out
                time.sleep(3)
        run(vm, "chmod +x /tmp/win/hello.exe /tmp/win/exitcode.exe /tmp/win/strings.exe")
        out, rc = run(vm, "/tmp/win/hello.exe")
        assert out.strip() == "Hello from a Windows program on TouhouOS!" and rc == 0, f"{rc} {out!r}"
        out, rc = run(vm, "/tmp/win/exitcode.exe")
        assert rc == 42, f"{rc} {out!r}"
        out, rc = run(vm, "cd /tmp/win && ./strings.exe a 'b c'")
        assert "argv[0] = [./strings.exe]\nargv[1] = [a]\nargv[2] = [b c]\n" in out and rc == 7, f"{rc}\n{out}"


FSOPS = """mkdir: 1
mkdir again: 0 error 183
dir attributes: directory 1
seek 3 from the start: 3, read [34]
seek -2 from the end: 1 8, read [89]
seek -4 from here: 6, read [67]
seek before the start: ffffffff error 131
file attributes: directory 0
missing: ffffffff error 2
delete: 1
delete again: 0 error 2
rmdir: 1
cwd: needs 1 more than its length: 1
to UTF-16: 16 units (16), [6771 65b9 d83c df38]
too small: 0 error 122
back to UTF-8: 22 bytes, same 1
"""
