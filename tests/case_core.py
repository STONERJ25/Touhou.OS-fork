"""Kernel and boot tests: self tests, user-space suite, kshell, keyboard, deliberate crashes."""
import re

from harness import test, TESTS, Qemu, BUILD


@test
def selftest():
    """In-kernel self tests: pmm, heap, vmm, scheduler."""
    with Qemu(cmdline="selftest", name="t-selftest") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=60)
        assert "SELFTEST PASS" in vm.text(), vm.text()[-800:]
        assert vm.wait_exit(10) == 0


@test
def utest():
    """User-space suite: libc, fork/exec/wait, pipes, signals, mmap, files, credentials, math."""
    with Qemu(cmdline="utest", name="t-utest") as vm:
        vm.expect(r"UTEST (PASS|FAIL)", timeout=90)
        assert "UTEST PASS" in vm.text(), vm.text()[-1500:]
        assert vm.wait_exit(10) == 0


@test
def kshell_serial():
    """kshell over serial: themed directory tree, initrd unpack, tmpfs create/read, symlinks."""
    with Qemu(cmdline="init=kshell", name="t-kshell-serial") as vm:
        vm.expect(r"kshell:# ", timeout=60)
        vm.send("ls /\n")
        vm.expect(r"Hakurei/.*HumanVillage/.*ScarletDevilMansion/.*YoukaiMountain/.*BambooForest/.*Netherworld/", timeout=10)
        vm.expect(r"kshell:# ")
        vm.send("write /tmp/hello.txt konnichiwa gensokyo\n")
        vm.expect(r"kshell:# ")
        vm.send("cat /BambooForest/hello.txt\n")
        vm.expect(r"konnichiwa gensokyo")
        vm.expect(r"kshell:# ")
        vm.send("ls -l /etc\n")
        vm.send("stat /home/reimu\n")
        vm.expect(r"mode 40755 uid 1000 gid 1000", timeout=10)
        vm.expect(r"kshell:# ")
        vm.send("mounts\n")
        vm.expect(r"devfs\s+on /dev.*procfs\s+on /proc|procfs.*on /proc", timeout=10)
        vm.expect(r"kshell:# ")
        vm.send("cat /proc/meminfo\n")
        vm.expect(r"MemTotal:\s+\d+ kB", timeout=10)


@test
def kshell_keyboard():
    """PS/2 keyboard path: keys injected through QMP reach the console tty and the line editor."""
    with Qemu(cmdline="init=kshell", name="t-kshell-kbd") as vm:
        vm.expect(r"kshell:# ", timeout=60)
        vm.type("echo Reimu is on duty\n")
        vm.expect(r"Reimu is on duty", timeout=15)
        vm.expect(r"kshell:# ")
        vm.type("echo wrongX")
        vm.key("backspace")
        vm.key("backspace")
        vm.type("ok\n")
        vm.expect(r"wrongok|wrong", timeout=10)
        vm.expect(r"kshell:# ")
        vm.shot = vm.screenshot(BUILD / "t-kshell-kbd.png")


@test
def shell():
    """gapsh and the kappabox userland (about 150 command checks driven over the serial console)."""
    import test_shell
    failures = test_shell.run_checks(verbose=False)
    assert not failures, f"{len(failures)} shell check(s) failed:\n" + "\n".join(failures[:6])


def crash_case(kind, expected):
    def run():
        with Qemu(cmdline=f"crash={kind}", name=f"t-crash-{kind}") as vm:
            vm.expect(r"INCIDENT: KERNEL PANIC", timeout=40)
            vm.expect(r"backtrace:\n(.*\n){2}", timeout=10)
            text = vm.text()
            assert re.search(expected, text), f"expected /{expected}/ in:\n{text[-1500:]}"
            assert "crash_test" in text or "kmain" in text, "backtrace should name kernel functions"
    run.__name__ = f"crash_{kind}"
    run.__doc__ = f"deliberate kernel fault '{kind}' must produce a diagnosable panic"
    TESTS.append(run)


crash_case("div0", r"Divide Error")
crash_case("pf", r"BOUNDARY VIOLATION: not-present page on write of 0xdead0000 by kernel")
crash_case("null", r"null pointer")
crash_case("gp", r"General Protection")
crash_case("ud", r"Invalid Opcode")
crash_case("nx", r"BOUNDARY VIOLATION: protection violation on instruction fetch")
crash_case("ro", r"BOUNDARY VIOLATION: protection violation on write")
crash_case("early-pf", r"BOUNDARY VIOLATION")
crash_case("panic", r"requested panic")
