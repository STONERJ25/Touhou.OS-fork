"""docs/COMPAT.md, "The personality is chosen by the file": execve looks at the ELF file (a PT_INTERP, the GNU/Linux OS/ABI byte, a GNU ABI note, or our own "Gensokyo"
note) and starts the program under the Linux or the native system call personality, whoever starts it.

The programs are the ones of tests/linux/src (built with gcc on a real Linux, tools/linux-oracle.py), the freestanding lnx_hello and the native programs of this build. The
markers are taken apart on the host: byte-patched copies of those programs that carry only one marker, or two that contradict each other, are written to build/linux-inputs and
downloaded into the guest like every other test input. Nothing of the Linux side is committed. Without WSL (or a Linux with gcc) these tests print SKIP and pass.

    python tools/qemu-slot.py -- python tests/run_all.py linux_exec
"""
import contextlib
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import test, Qemu, BUILD
from case_linux import Inputs, build_inputs, compare_with_linux, TRACE, INPUTS
from case_linux_dyn import World, skip_reason, build_root, root_run, fetch_root, ROOT_PROMPT

_inputs_built = False


def ensure_inputs():
    """build/linux-inputs (the test programs, gcc on a real Linux) takes a minute: once per run of the tests, not once per guest."""
    global _inputs_built
    if not _inputs_built:
        build_inputs()
        _inputs_built = True


class Guest(World):
    """A World (a root shell, the programs in /tmp, the Linux root) that builds the inputs only once per run, and can leave the unpacking of the root for later:
    Guest(name, files, late_root=True) has /tmp/linux-root.tar and says nothing is mounted until guest.mount_root()."""

    def __init__(self, name, files=(), late_root=False, **kw):
        super().__init__(name, files, root=not late_root, **kw)
        self.late_root = late_root

    def __enter__(self):
        ensure_inputs()
        if self.root or self.late_root:
            build_root()
        self.stack = contextlib.ExitStack()
        try:
            self.srv = self.stack.enter_context(Inputs())
            self.vm = self.stack.enter_context(Qemu(cmdline=self.cmdline + TRACE, name=self.name, net="virtio-net-pci", mem=self.mem))
            self.vm.expect(ROOT_PROMPT, timeout=300)
            for n in self.files + self.extra + (["linux-root.tar"] if self.root or self.late_root else []):
                fetch_root(self.vm, self.srv.port, n)
            if self.root:
                self.mount_root()
        except BaseException:
            self.stack.close()
            raise
        return self

    def mount_root(self):
        t0 = time.time()
        out, rc = root_run(self.vm, "linuxroot /tmp/linux-root.tar", timeout=300)
        assert rc == 0, f"linuxroot failed ({rc}): {out}"
        self.unpack_seconds = time.time() - t0
        root_run(self.vm, "rm /tmp/linux-root.tar")

    def download(self, name, to):
        """/tmp/NAME of the guest, but saved as `to` (a path: for a program that prints its own file name)."""
        out, rc = root_run(self.vm, f"fetch -q -O {to} http://10.0.2.2:{self.srv.port}/{name} && chmod +x {to}", timeout=120)
        assert rc == 0, f"could not download {name} as {to}: {out!r}"


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
# files that carry one marker, or two that contradict each other

EI_OSABI = 7
NATIVE_NOTE = struct.pack("<III", 9, 4, 1) + b"Gensokyo\0\0\0\0"                   # namesz, descsz, type NT_GENSOKYO_ABI, the name padded to a multiple of 4 (user/user.ld)
GNU_ABI_TAG = struct.pack("<III", 4, 16, 1) + b"GNU\0"                              # the header of NT_GNU_ABI_TAG: owner "GNU", a 16-byte descriptor (os, major, minor, patch)


def with_osabi(data, value):
    b = bytearray(data)
    b[EI_OSABI] = value
    return bytes(b)


def corrupt(data, pattern, owner_at, new):
    """data with the owner name inside the one note header `pattern` replaced by `new` (the same length): the note is still there but names someone else."""
    assert data.count(pattern) == 1, f"the note header {pattern!r} should occur once, it occurs {data.count(pattern)} times"
    return data.replace(pattern, pattern[:owner_at] + new + pattern[owner_at + len(new):])


def make_marker_files():
    """Copies of real programs with exactly the markers a test wants, in build/linux-inputs (served to the guest). Returns nothing; the names are the file names."""
    INPUTS.mkdir(parents=True, exist_ok=True)
    native = (BUILD / "user" / "bin" / "hello").read_bytes()                        # a native program: the Gensokyo note, no other marker
    assert native.count(NATIVE_NOTE) == 1 and native[EI_OSABI] == 0, "build/user/bin/hello should carry the native note (python tools/build.py first)"
    lnx = (BUILD / "user" / "linuxtest" / "lnx_hello").read_bytes()                 # freestanding, Linux system calls, no marker at all
    glibc = (INPUTS / "glibc_hello").read_bytes()                                   # static glibc: OS/ABI GNU and the ABI note
    dyn = (INPUTS / "dyn_hello").read_bytes()                                       # dynamic PIE: PT_INTERP and the ABI note (OS/ABI 0)
    assert glibc[EI_OSABI] == 3 and glibc.count(GNU_ABI_TAG) == 1 and dyn[EI_OSABI] == 0 and dyn.count(GNU_ABI_TAG) == 1, "unexpected markers in the glibc programs"
    files = {
        "x_both": with_osabi(native, 3),                                            # native note + OS/ABI GNU: claims to be both
        "x_nomark": corrupt(native, NATIVE_NOTE, 12, b"Gensokyx"),                  # the note names another owner: no marker
        "x_plain": lnx,                                                             # a Linux program with no marker (never started by name: the native shell would run it natively)
        "x_osabi": with_osabi(lnx, 3),                                              # only e_ident[EI_OSABI] says Linux
        "x_tag": with_osabi(glibc, 0),                                              # only the GNU ABI note says Linux
        "x_interp": corrupt(dyn, GNU_ABI_TAG, 12, b"GNX"),                          # only PT_INTERP says Linux
    }
    for name, data in files.items():
        (INPUTS / name).write_bytes(data)
    return list(files)


MARKED = ["x_both", "x_nomark", "x_plain", "x_osabi", "x_tag", "x_interp"]


def golden_run(w, golden, cmd, timeout=90):
    """cmd in the guest prints what the stored output of a real Linux says for `golden` (and ends with the same status)."""
    out, rc = w.run(cmd, timeout=timeout)
    compare_with_linux(golden, out, rc, ("[Outside World]",))


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

@test
def linux_exec_native_shell_starts_linux_programs():
    """A Linux program is started from the native shell by its name, with no `linux` in front: the file decides. A static glibc program (OS/ABI GNU and the ABI note), a
    program with only the ABI note, a freestanding one with only OS/ABI GNU, all with no Linux root mounted (they run with real paths), then, with the root mounted, a dynamic
    PIE and one with only a PT_INTERP: each prints what a real Linux prints. The native programs keep working, a file with both kinds of marker is refused, and one with none
    keeps the native shell's personality"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ensure_inputs()
    make_marker_files()
    with Guest("t-linux-exec-shell", ["glibc_hello", "dyn_hello", "glibc_libc"] + MARKED, late_root=True) as w:
        # no Linux root yet: Linux programs run with real paths
        out, rc = w.run("linuxroot; echo")
        assert "no Linux root" in out, out
        golden_run(w, "glibc_hello", "/tmp/glibc_hello")
        golden_run(w, "glibc_hello", "/tmp/x_tag")                          # only the GNU ABI note
        golden_run(w, "lnx_hello", "/tmp/x_osabi")                        # only OS/ABI GNU; a freestanding program
        golden_run(w, "glibc_libc", "FOO=bar /tmp/glibc_libc alpha beta", timeout=180)
        # the native side
        out, rc = w.run("/tmp/x_nomark alpha")                                       # no marker: the native shell's personality
        assert rc == 0 and "Hello from Gensokyo, userland!" in out and "argv[1] = alpha" in out, f"an unmarked native program: {rc} {out!r}"
        out, rc = w.run("/bin/hello")
        assert rc == 0 and "Hello from Gensokyo, userland!" in out, f"a native program: {rc} {out!r}"
        out, rc = w.run("/tmp/x_both 2>&1")
        assert rc != 0 and "Hello" not in out, f"a file with both kinds of marker must not run: {rc} {out!r}"
        print(f"     (both markers: status {rc}, {out.strip()!r})")

        # the root is mounted now: the Linux program enters it, and its dynamic linker is found there
        w.mount_root()
        golden_run(w, "dyn_hello", "FOO=bar /tmp/dyn_hello alpha beta")
        golden_run(w, "glibc_hello", "/tmp/glibc_hello")
        w.run("mkdir -p /tmp/interp")
        w.download("x_interp", "/tmp/interp/dyn_hello")                              # only PT_INTERP says Linux (the program prints its own file name: keep it)
        golden_run(w, "dyn_hello", "FOO=bar /tmp/interp/dyn_hello alpha beta")
        out, rc = w.run("cd /tmp && FOO=bar ./dyn_hello alpha beta | head -1")        # a relative name, found from the working directory
        assert out == "hello from a dynamic PIE program\n", out
        out, rc = w.run("/tmp/x_both 2>&1")
        assert rc != 0 and "Hello" not in out, f"both markers, with a root: {rc} {out!r}"
        # an explicit request: `linux` makes a program with no marker a Linux program (and reads its path in the Linux root), for that one exec
        golden_run(w, "lnx_hello", "linux /tmp/x_plain")


@test
def linux_exec_native_tools_from_a_linux_shell():
    """A Linux shell starts a native tool by its path: the path is read in the Linux root (so the tool is in /tmp, one of the five directories of the machine), the tool runs under
    the native personality (uname says Gensokyo, /proc/self/status has the native format) outside the Linux root, keeps the working directory when that is visible, hands back
    its exit status, and works in a pipeline with a Linux program. A `#!` script follows its interpreter: the same script prints Gensokyo when the native shell runs it and Linux
    when a Linux shell does, and a native shell that runs a script whose interpreter is a Linux program runs it the Linux way"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with Guest("t-linux-exec-tools", []) as w:
        w.run("cp /bin/kappabox /tmp/kappabox && chmod +x /tmp/kappabox")
        sh = "linux /bin/sh -c"
        out, rc = w.run(f"{sh} '/tmp/kappabox uname -s'")
        assert (out, rc) == ("Gensokyo\n", 0), f"a native tool run by a Linux shell: {rc} {out!r}"
        out, rc = w.run(f"{sh} '/bin/uname -s'")
        assert (out, rc) == ("Linux\n", 0), f"the Linux uname: {rc} {out!r}"
        out, rc = w.run(f"{sh} 'cd /tmp && /tmp/kappabox readlink /proc/self/cwd'")
        assert (out, rc) == ("/tmp\n", 0), f"the working directory is kept when it is visible on both sides: {rc} {out!r}"
        out, rc = w.run(f"{sh} 'cd /usr && /tmp/kappabox readlink /proc/self/cwd'")
        assert (out, rc) == ("/\n", 0), f"a directory of the Linux tree is not visible to a native program: it starts in /: {rc} {out!r}"
        out, rc = w.run(f"{sh} '/tmp/kappabox false; echo status=$?'")
        assert (out, rc) == ("status=1\n", 0), f"the native tool's exit status: {rc} {out!r}"
        out, rc = w.run(f"{sh} '/tmp/kappabox cat /proc/self/status'")
        assert rc == 0 and "\nPGid:\t" in out and out.startswith("Name:\t") and "TracerPid" not in out, f"/proc/self/status in the native format: {rc} {out!r}"
        out, rc = w.run(f"{sh} '/tmp/kappabox ls / | /bin/cat'")
        assert rc == 0 and "Hakurei" in out.split(), f"a native program sees the real root (and a pipe joins it to a Linux program): {rc} {out!r}"
        out, rc = w.run(f"{sh} '/bin/ls / | /tmp/kappabox wc -l'")
        assert rc == 0 and int(out.strip()) >= 10, f"a Linux program feeds a native one: {rc} {out!r}"

        # scripts follow their interpreter
        w.run("printf '#!/bin/sh\\nuname -s\\n' > /tmp/s_uname.sh && chmod +x /tmp/s_uname.sh")
        out, rc = w.run("/tmp/s_uname.sh")
        assert (out, rc) == ("Gensokyo\n", 0), f"a script run by the native shell, interpreter /bin/sh (the native one): {rc} {out!r}"
        out, rc = w.run(f"{sh} /tmp/s_uname.sh")
        assert (out, rc) == ("Linux\n", 0), f"the same script run by a Linux shell: its /bin/sh is the root's dash: {rc} {out!r}"
        w.run("cp /linux/usr/bin/dash /tmp/dash_copy && chmod +x /tmp/dash_copy && printf '#!/tmp/dash_copy\\nuname -s\\n' > /tmp/s_linux.sh && chmod +x /tmp/s_linux.sh")
        out, rc = w.run("/tmp/s_linux.sh")
        assert (out, rc) == ("Linux\n", 0), f"a script that names a Linux interpreter, started by the native shell: {rc} {out!r}"


@test
def linux_exec_inherit_switch_restores_the_old_rule():
    """exec.abi=inherit on the kernel command line turns the markers off: a dynamic Linux program started by name from the native shell is a native process again and cannot
    be loaded (a position-independent file is refused by the native loader, as it was before this rule), while `linux PROGRAM` still works; native programs are unaffected, and the
    kernel log says the switch is on"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with Guest("t-linux-exec-inherit", ["dyn_hello"], cmdline="text user=root exec.abi=inherit") as w:
        out, rc = w.run("dmesg | grep -c 'no longer chosen by the file'")
        assert out.strip() == "1", f"the kernel log should say the markers are off: {out!r}"
        out, rc = w.run("FOO=bar /tmp/dyn_hello alpha beta 2>&1")
        assert rc != 0 and "hello from" not in out, f"without the markers a Linux PIE cannot start from the native shell: {rc} {out!r}"
        golden_run(w, "dyn_hello", "FOO=bar linux /tmp/dyn_hello alpha beta")
        out, rc = w.run("/bin/hello")
        assert rc == 0 and "Hello from Gensokyo, userland!" in out, f"{rc} {out!r}"
