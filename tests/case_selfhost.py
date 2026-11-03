"""The system builds itself (docs/SELFHOST.md): mmake, the file times a build depends on, and the userland rebuilt inside the guest.

  selfhost_mmake_guest        mmake runs a small C project in the guest: builds, says "up to date", rebuilds exactly what a touched file needs, sees
                              the times that `touch -t` / `touch -r` set, and `make` is the same program
  selfhost_rebuild_userland   libgap, kappabox, mmake and mcc are rebuilt from the sources in the initrd by `make` in a guest; the programs are
                              installed, run, copied to a disk, and a second guest boots with the rebuilt kappabox in its initrd and passes the shell
                              test table (the 95-line one that tests/case_mcc.py uses too)
  selfhost_touch_fat          touch and mmake on a FAT volume: the two-second granularity of the directory entries (the host reads the image afterwards)

mmake itself is checked against GNU make on the host by tests/host/test_mmake.py.  Run the guest tests through the limiter:
  python tools/qemu-slot.py -- python tests/run_all.py selfhost_
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import test, Qemu, BUILD  # noqa: E402
import mcc_util as U  # noqa: E402

ROOT_PROMPT = r"root@gensokyo \S+ # "
USER_PROMPT = U.PROMPT

PROJECT = {
    "selfhost-test/t.c": (b"#include <stdio.h>\n#include <string.h>\n"
                          b"int main(void) { char b[16]; strcpy(b, \"libgap\"); printf(\"%s %d\\n\", b, (int)strlen(b)); return 0; }\n"),
    "selfhost-test/a.c": b"int helper(void);\nint main(void) { return helper(); }\n",
    "selfhost-test/b.c": b"int helper(void) { return 7; }\n",
    "selfhost-test/Makefile": (b"OBJS = a.o b.o\nprog: $(OBJS)\n\tcc -o $@ $(OBJS)\n%.o: %.c\n\tcc -c -o $@ $<\n"
                               b"clean:\n\trm -f prog $(OBJS)\n.PHONY: clean\n"),
}


class Sh:
    """One shell line at a time, with the markers of tests/test_shell.py; `root` picks the prompt."""

    def __init__(self, vm, root=False):
        self.vm, self.prompt = vm, ROOT_PROMPT if root else USER_PROMPT

    def run(self, cmd, timeout=60):
        out, rc = U.run(self.vm, cmd, timeout=timeout, prompt=self.prompt)
        return out, rc

    def ok(self, cmd, timeout=60):
        out, rc = self.run(cmd, timeout)
        assert rc == 0, f"`{cmd}` exited with {rc}:\n{out[-2500:]}"
        return out


def lines(text):
    return [ln.rstrip() for ln in text.replace("\r", "").splitlines() if ln.strip()]


@test
def selfhost_mmake_guest():
    """mmake in the guest: a small C project builds, is up to date, rebuilds what a touched file needs, and obeys touch -t / -r; `make` is mmake."""
    esp = U.make_esp("t-selfhost-mmake", {k: v for k, v in PROJECT.items()})
    with Qemu(cmdline="text", name="t-selfhost-mmake", esp=esp, mem="768M") as vm:
        vm.expect(USER_PROMPT, timeout=300)
        sh = Sh(vm)
        out = sh.ok("make --version; mmake --version; ls -l /bin/make")
        assert out.count("mmake 0.1") == 2 and "make" in out and "mmake" in out.splitlines()[-1], f"step 1: {out}"
        sh.ok("cp -r /selfhost-test /tmp/p && cd /tmp/p")
        # the sources are dated 2000, the objects built below get today's time: nothing depends on how fast this PC is (file times are whole seconds)
        sh.ok("touch -t 200001010000 a.c b.c Makefile")
        out = sh.ok("make", timeout=120)
        assert lines(out) == ["cc -c -o a.o a.c", "cc -c -o b.o b.c", "cc -o prog a.o b.o"], f"step 2: {out}"
        out, rc = sh.run("./prog")
        assert rc == 7, f"step 3: {(out, rc)}"
        out = sh.ok("make")
        assert lines(out) == ["make: 'prog' is up to date."], f"step 4: {out}"
        # an object older than its source is rebuilt, and what depends on it, nothing else
        sh.ok("touch -t 199901010000 a.o")
        out = sh.ok("make -n")
        assert lines(out) == ["cc -c -o a.o a.c", "cc -o prog a.o b.o"], f"step 5: {out}"
        out = sh.ok("make", timeout=120)
        assert lines(out) == ["cc -c -o a.o a.c", "cc -o prog a.o b.o"], f"step 6: {out}"
        # touch -t sets the time (local time, UTC here); an object dated after its source is up to date, one dated before it is not
        sh.ok("touch -t 200001020304.05 b.o")
        out = sh.ok("stat b.o")
        assert "2000-01-02 03:04:05" in out, f"step 7: {out}"
        out = sh.ok("make")
        assert lines(out) == ["make: 'prog' is up to date."], f"step 8: {out}"
        sh.ok("touch -t 199912312359 b.o")
        out = sh.ok("make", timeout=120)
        assert lines(out) == ["cc -c -o b.o b.c", "cc -o prog a.o b.o"], f"step 8b: {out}"
        # touch -r copies the time of another file; a prerequisite exactly as old as the target does not rebuild it
        sh.ok("touch -r a.c prog a.o b.o")
        out = sh.ok("make")
        assert lines(out) == ["make: 'prog' is up to date."], f"step 9: {out}"
        # touch -c does not create, plain touch does; a missing target is built, a phony one always runs
        out, rc = sh.run("touch -c ghost; ls ghost")
        assert "No such file" in out, f"step 10: {out}"
        sh.ok("touch fresh && test -f fresh")
        out = sh.ok("make clean && ls")
        assert lines(out)[0] == "rm -f prog a.o b.o" and "prog" not in lines(out)[1:], f"step 11: {out}"
        out = sh.ok("make -n -s prog")
        assert lines(out) == ["cc -c -o a.o a.c", "cc -c -o b.o b.c", "cc -o prog a.o b.o"], f"step 12: {out}"
        # errors read like GNU make's
        out, rc = sh.run("make nosuchthing")
        assert rc == 2 and "No rule to make target 'nosuchthing'.  Stop." in out, f"step 13: {(out, rc)}"
        out, rc = sh.run("printf 'x:\\n\\tfalse\\n' > bad.mk; make -f bad.mk")
        assert rc == 2 and "[x] Error 1" in out, f"step 14: {(out, rc)}"


@test
def selfhost_rebuild_userland():
    """libgap, kappabox, mmake and mcc are rebuilt by `make` in a guest from the sources in the initrd; the result runs, builds a program, and a second guest boots with the rebuilt kappabox."""
    import test_shell
    from case_storage import Image, hv_part, MIB
    esp = U.make_esp("t-selfhost-a", PROJECT)
    with Image("selfhost", 96 * MIB, [hv_part({})]) as img:
        with Qemu(cmdline="text user=root bootlog=off", disk=img.path, esp=esp, name="t-selfhost-a", mem="1024M") as vm:
            vm.expect(ROOT_PROMPT, timeout=300)
            sh = Sh(vm, root=True)
            out = sh.ok("cd /ScarletDevilMansion/src && ls && ls libc | head -3 && ls kappabox | wc -l")
            for name in ("Makefile", "gen-applets.awk", "libc", "kappabox", "mmake", "mcc", "mas"):
                assert name in out, f"/ScarletDevilMansion/src has no {name}:\n{out}"
            sh.ok("mkdir -p /HumanVillage/selfhost")
            # the whole build, in the background of the console: one line of output per tool call goes to a log
            out = sh.ok("cd /ScarletDevilMansion/src && make OUT=/tmp/build > /tmp/make.log 2>&1; echo make-rc=$?; tail -4 /tmp/make.log | awk '{print substr($0, 1, 300)}'", timeout=3000)
            assert "make-rc=0" in out, out
            log = sh.ok("grep -c '^mcc ' /tmp/make.log; grep -c ': error: ' /tmp/make.log; ls -l /tmp/build/lib /tmp/build/bin")
            assert "libgap.a" in log and "kappabox" in log and "mmake" in log and "mcc" in log, log
            assert log.splitlines()[1].strip() == "0", "mcc reported errors while building:\n" + sh.ok("grep ': error: ' /tmp/make.log | head")
            out = sh.ok("grep -c '^/bin/mas ' /tmp/make.log; ls -l /tmp/build/bin/mas")
            assert out.splitlines()[0].strip() == "2" and "/tmp/build/bin/mas" in out, f"crt0 and setjmp should have been assembled by mas:\n{out}"
            # nothing left to do, and the right things rebuild when a source changes (the mtimes of tmpfs files decide)
            out = sh.ok("cd /ScarletDevilMansion/src && make OUT=/tmp/build libgap kappabox")
            assert "up to date" in out or "Nothing to be done" in out, out
            sh.ok("touch -t 203001010000 /ScarletDevilMansion/src/libc/string.c")
            out = sh.ok("cd /ScarletDevilMansion/src && make -n OUT=/tmp/build libgap kappabox")
            todo = [ln for ln in lines(out) if not ln.startswith("mkdir") and not ln.startswith("rm ")]
            assert any(" string.c " in ln or ln.endswith("string.c -o /tmp/build/obj/libc/string.o") for ln in todo), out
            assert sum(1 for ln in todo if " -c " in ln) == 1, f"only string.o should be compiled again:\n{out}"
            assert any("libgap.a" in ln and "mar" in ln for ln in todo) and any("kappabox" in ln and "-nostdlib" in ln for ln in todo), out
            # install, and run the rebuilt programs
            sh.ok("cd /ScarletDevilMansion/src && make OUT=/tmp/build install DESTDIR=/tmp/newroot > /tmp/install.log 2>&1; ls -l /tmp/newroot/bin", timeout=1500)
            out = sh.ok("/tmp/newroot/bin/kappabox echo hello from the rebuilt kappabox | /tmp/newroot/bin/kappabox sed s/hello/greetings/")
            assert "greetings from the rebuilt kappabox" in out, out
            out = sh.ok("printf 'b 2\\na 1\\n' | /tmp/newroot/bin/kappabox sort | /tmp/newroot/bin/kappabox awk '{print $2 $1}'")
            assert "1a" in out and "2b" in out, out
            out = sh.ok("/tmp/newroot/bin/mmake --version && /tmp/newroot/bin/mcc --version")
            assert "mmake 0.1" in out and "mcc 0.1" in out, out
            # a program linked against the rebuilt libgap and the assembler objects of the rebuilt tree
            sh.ok("cp /selfhost-test/t.c /tmp/t.c && /tmp/newroot/bin/mcc -nostdlib /tmp/build/obj/libc/crt0.o /tmp/t.c /tmp/build/lib/libgap.a -o /tmp/t")
            out = sh.ok("/tmp/t")
            assert "libgap 6" in out, out
            # the rebuilt compiler compiles itself to the same bytes as the compiler that built it
            sh.ok("/tmp/newroot/bin/mcc -nostdlib /tmp/build/obj/libc/crt0.o /ScarletDevilMansion/src/mcc/main.c /tmp/build/lib/libgap.a -o /tmp/mcc-again", timeout=900)
            out = sh.ok("cmp /tmp/mcc-again /tmp/build/bin/mcc && echo mcc-same")
            assert "mcc-same" in out, out
            # to the disk for the host, then the machine stops cleanly
            sh.ok("cp /tmp/newroot/bin/kappabox /HumanVillage/selfhost/kappabox && cp /tmp/build/lib/libgap.a /HumanVillage/selfhost/libgap.a && sync")
            vm.send("poweroff\n")
            assert vm.wait_exit(60) is not None, "the guest did not power off:\n" + vm.text()[-1500:]
        img.check()
        vol = img.volume(1)
        try:
            kb = vol.read_file("/selfhost/kappabox")
            lib = vol.read_file("/selfhost/libgap.a")
        finally:
            vol.close()
        assert kb[:4] == b"\x7fELF" and len(kb) > 500_000, f"kappabox on the disk is {len(kb)} bytes"
        assert lib[:8] == b"!<arch>\n" and len(lib) > 200_000, f"libgap.a on the disk is {len(lib)} bytes"
        rebuilt = BUILD / "selfhost-kappabox"
        rebuilt.write_bytes(kb)
    # a second guest boots with the rebuilt kappabox in its initrd (every applet of the system is that one program) and passes the shell test table
    esp_b = U.make_esp("t-selfhost-b", {"bin/kappabox": rebuilt})
    failures = []
    with Qemu(cmdline="text", name="t-selfhost-b", esp=esp_b, mem="768M") as vm:
        vm.expect(USER_PROMPT, timeout=300)
        for cmd, expect, status in test_shell.CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                assert status is None or rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-500:]}")
    assert not failures, "\n".join(failures)


ASM_FILES = ["user/libc/crt0.asm", "user/libc/src/setjmp.asm", "kernel/arch/x86_64/ksyms.asm", "kernel/arch/x86_64/efi_call.asm", "kernel/arch/x86_64/entry.asm",
             "kernel/arch/x86_64/uaccess.asm", "kernel/arch/x86_64/switch.asm", "kernel/arch/x86_64/syscall.asm", "kernel/arch/x86_64/isr.asm"]


def find_nasm():
    import os
    import shutil
    for cand in (os.environ.get("TOUHOU_NASM"), shutil.which("nasm"), os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM\nasm.exe")):
        if cand and Path(cand).is_file():
            return cand
    return None


@test
def selfhost_mas_guest():
    """mas in the guest assembles the nine .asm files of the kernel and libc to the same bytes NASM writes for them (the references come from NASM on the host)."""
    import subprocess
    import tempfile
    nasm = find_nasm()
    assert nasm, "this test needs NASM on the host (TOUHOU_NASM, the PATH or tools/build.py's install folder)"
    extra = {}
    with tempfile.TemporaryDirectory(prefix="mas-ref-") as tmp:
        tmp = Path(tmp)
        (tmp / "ksyms.bin").write_bytes(bytes(range(256)) * 4)
        extra["selfhost-asm/ksyms.bin"] = (tmp / "ksyms.bin").read_bytes()
        for rel in ASM_FILES:
            name = Path(rel).name
            (tmp / name).write_bytes((U.ROOT / rel).read_bytes())
            r = subprocess.run([nasm, "-f", "elf64", name, "-o", name[:-4] + ".o"], cwd=tmp, capture_output=True, text=True)
            assert r.returncode == 0, f"nasm failed on {rel}: {r.stderr}"
            extra[f"selfhost-asm/{name}"] = (tmp / name).read_bytes()
            extra[f"selfhost-asm/ref/{name[:-4]}.o"] = (tmp / (name[:-4] + ".o")).read_bytes()
    esp = U.make_esp("t-selfhost-mas", extra)
    failures = []
    with Qemu(cmdline="text", name="t-selfhost-mas", esp=esp, mem="768M") as vm:
        vm.expect(USER_PROMPT, timeout=300)
        sh = Sh(vm)
        out = sh.ok("mas --version; cd /selfhost-asm && ls")
        assert "mas 0.1" in out, out
        for rel in ASM_FILES:
            stem = Path(rel).name[:-4]
            out, rc = sh.run(f"cd /selfhost-asm && mas -I /selfhost-asm/ {stem}.asm -o /tmp/{stem}.o && cmp /tmp/{stem}.o ref/{stem}.o && echo same-{stem}", timeout=120)
            if f"same-{stem}" not in out:
                failures.append(f"{stem}: rc={rc}\n{out[-800:]}")
        # errors name the line, and a failed assembly writes no object
        sh.ok("printf 'bits 64\\nsection .text\\n  frobnicate rax\\n' > /tmp/bad.asm")
        out, rc = sh.run("mas /tmp/bad.asm -o /tmp/bad.o; echo rc=$?; ls /tmp/bad.o 2>&1")
        if not ("/tmp/bad.asm:3: error" in out and "rc=1" in out and "No such file" in out):
            failures.append("error handling:\n" + out)
    assert not failures, "\n\n".join(failures)


@test
def selfhost_touch_fat():
    """touch and mmake on a FAT volume: the times land in the directory entry in two-second steps, make's decisions follow them."""
    from case_storage import Image, hv_part, MIB
    esp = U.make_esp("t-selfhost-fat", {k: v for k, v in PROJECT.items()})
    with Image("selfhost-fat", 64 * MIB, [hv_part({})]) as img:
        with Qemu(cmdline="text user=root bootlog=off", disk=img.path, esp=esp, name="t-selfhost-fat", mem="768M") as vm:
            vm.expect(ROOT_PROMPT, timeout=300)
            sh = Sh(vm, root=True)
            sh.ok("cp -r /selfhost-test /HumanVillage/p && cd /HumanVillage/p")
            out = sh.ok("make", timeout=300)
            assert lines(out) == ["cc -c -o a.o a.c", "cc -c -o b.o b.c", "cc -o prog a.o b.o"], out
            # a time with odd seconds is kept as the even second below it; the guest sees what the entry holds
            sh.ok("touch -t 202001020304.05 a.c")
            out = sh.ok("stat a.c")
            assert re.search(r"2020-01-02 03:04:0[45]", out), out
            out = sh.ok("make -n")
            assert lines(out) == ["make: 'prog' is up to date."], out          # a.c became older than a.o
            sh.ok("touch -t 203001020304 a.c")
            out = sh.ok("make -n")
            assert lines(out) == ["cc -c -o a.o a.c", "cc -o prog a.o b.o"], out
            sh.ok("touch -t 200001010000 prog a.o b.o && sync")
            vm.send("poweroff\n")
            assert vm.wait_exit(60) is not None
        img.check()
        vol = img.volume(1)
        try:
            a = vol.find("/p/a.c")
            assert a is not None and a.mtime == 1893553440, f"a.c: {a and a.mtime} on disk (2030-01-02 03:04:00 UTC = 1893553440)"
            prog = vol.find("/p/prog")
            assert prog is not None and prog.mtime == 946684800, f"prog: {prog and prog.mtime} on disk (2000-01-01 00:00:00 UTC = 946684800)"
        finally:
            vol.close()
