"""Recovery without a person (docs/RELIABILITY.md): the TCO watchdog and who feeds it, the panic record that outlives a reset, the netconsole.

QEMU's q35 has the ICH9's TCO watchdog. With -no-reboot (the harness's default) the reset it causes ends QEMU: that is the signal. The tests that
need the next start too run with no_reboot=False: QEMU keeps the guest's RAM through a reset, as most PCs keep DRAM through a warm one. The
netconsole's receiver is tools/netconsole.py on the host's loopback, which QEMU's user network shows the guest as 10.0.2.2.

  python tests/run_all.py reliability_
"""
import re
import shutil
import sys
import time
import uuid
from pathlib import Path

import test_shell
from harness import test, Qemu, BUILD, scaled

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from netconsole import Receiver  # noqa: E402

ROOT = r"root@gensokyo \S+ # "


def root(vm, cmd, timeout=60):
    return test_shell.run(vm, cmd, timeout=timeout, prompt=ROOT)


def feeder_pid(vm):
    out, _ = root(vm, "cat /proc/watchdog")
    m = re.search(r"'wdt-feeder' \(pid (\d+)\)", out)
    assert m, "user space does not feed the watchdog:\n" + out
    return int(m.group(1))


def wait_feeder(vm, timeout=30):
    """The feeder opens /dev/watchdog shortly after init starts; the prompt may come first."""
    deadline = time.time() + scaled(timeout)
    while True:
        out, _ = root(vm, "cat /proc/watchdog")
        if "'wdt-feeder'" in out:
            return out
        assert time.time() < deadline, "init's feeder never took the watchdog over:\n" + out
        time.sleep(1)


def stays_alive(vm, seconds, what):
    time.sleep(scaled(seconds))
    assert vm.alive(), f"the machine restarted: {what}\n" + vm.text()[-2000:]
    out, rc = root(vm, "echo still-here")
    assert "still-here" in out, out


def private_esp(name, kernel=None, extra=None):
    """A copy of build/esp in build/<name>: the kernel replaced by `kernel` (bytes) if given, more files from `extra` ({name: bytes})."""
    src, esp = BUILD / "esp", BUILD / name
    if esp.exists():
        shutil.rmtree(esp)
    (esp / "EFI" / "BOOT").mkdir(parents=True)
    shutil.copy2(src / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    shutil.copy2(src / "initrd.img", esp / "initrd.img")
    (esp / "kernel.elf").write_bytes(kernel if kernel is not None else (src / "kernel.elf").read_bytes())
    for k, v in (extra or {}).items():
        (esp / k).write_bytes(v)
    return esp


def trial_files(old_cmdline):
    """What a pushed update on trial leaves on the ESP: update.try (fresh contents, so the loader's start counter, kept in the VM's
    variable store across runs, begins at 1), the previous kernel, initrd and command line as *.old."""
    src = BUILD / "esp"
    return {"update.try": f"version reliability-test\nnonce {uuid.uuid4()}\n".encode(), "kernel.old": (src / "kernel.elf").read_bytes(),
            "initrd.old": (src / "initrd.img").read_bytes(), "cmdline.old": (old_cmdline + " nosounds nosshd\n").encode()}


def hung_kernel():
    """build/esp's kernel with a jump to itself at its entry point (it still carries the feeder mark, so the loader arms the watchdog for it)."""
    k = bytearray((BUILD / "esp" / "kernel.elf").read_bytes())
    entry = int.from_bytes(k[24:32], "little")
    phoff, phentsize, phnum = int.from_bytes(k[32:40], "little"), int.from_bytes(k[54:56], "little"), int.from_bytes(k[56:58], "little")
    for i in range(phnum):
        ph = phoff + i * phentsize
        p_type = int.from_bytes(k[ph:ph + 4], "little")
        p_offset, p_vaddr, p_filesz = (int.from_bytes(k[ph + o:ph + o + 8], "little") for o in (8, 16, 32))
        if p_type == 1 and p_vaddr <= entry < p_vaddr + p_filesz:
            k[p_offset + entry - p_vaddr:p_offset + entry - p_vaddr + 2] = b"\xeb\xfe"          # jmp .
            return bytes(k)
    raise AssertionError("no segment holds the kernel's entry point")


# ---------------------------------------------------------------- the watchdog


@test
def reliability_wdt_feeders():
    """the loader arms the TCO watchdog before the kernel starts, the kernel takes it over, init's feeder takes it over from the kernel through
    /dev/watchdog; NO_REBOOT is clear on q35 and /proc/watchdog says all of it; a machine whose user space runs is never restarted"""
    with Qemu(cmdline="text user=root wdt=6", name="t-rel-wdt-feeders") as vm:
        vm.expect(ROOT, timeout=120)
        log = vm.text()
        assert re.search(r"loader: the TCO watchdog is armed: 6 s \(TCO at io 0x0*660\)", log), log[-3000:]
        out = wait_feeder(vm)
        for want in ("chipset:  ICH6..9-series", "tco:      io 0x660, ACPI block io 0x600, NO_REBOOT: bit 5 of the register at 0xfed1f410", "state:    armed, 6 s",
                     "reset:    possible (NO_REBOOT is clear)", "loader:   armed it before the kernel started (6 s)", "user space (/dev/watchdog): 'wdt-feeder'"):
            assert want in out, (want, out)
        dm, _ = root(vm, "dmesg | grep Momiji")
        assert "the watchdog is armed: 6 s, the loader armed it, the kernel takes it over" in dm, dm
        assert re.search(r"'wdt-feeder' \(pid \d+\) feeds the watchdog from now on", dm), dm
        assert "crw-------" in root(vm, "ls -l /dev/watchdog")[0]
        stays_alive(vm, 20, "with its feeder running")
        out, _ = root(vm, "cat /proc/watchdog")
        assert "late:     0 first timeouts" in out and "BLOCKED" not in out, out


@test
def reliability_wdt_hung_userland_restarts():
    """a user space that no longer runs its feeder (SIGSTOP stands in for a hung one) lets the timer run out: the machine restarts within the
    timeout (QEMU, with -no-reboot, exits), and the kernel said that nobody was feeding it"""
    with Qemu(cmdline="text user=root wdt=6", name="t-rel-wdt-hung") as vm:
        vm.expect(ROOT, timeout=120)
        wait_feeder(vm)
        pid = feeder_pid(vm)
        t0 = time.time()
        vm.send(f"kill -STOP {pid}\n")
        assert vm.wait_exit(90) is not None, "the watchdog did not restart the machine"
        took = time.time() - t0
        assert took >= 3, f"the machine went after {took:.1f} s: before the watchdog could have"
        assert "nobody has fed it for" in vm.text(), vm.text()[-1500:]


@test
def reliability_wdt_kernel_feeds_until_user_space_does():
    """without a user-space feeder (init=kshell: no init at all) the kernel's Momiji thread feeds the watchdog for good"""
    with Qemu(cmdline="init=kshell wdt=4", name="t-rel-wdt-kshell") as vm:
        vm.expect(r"kshell:# ", timeout=90)
        time.sleep(scaled(20))
        assert vm.alive(), "the machine restarted although the kernel was feeding the watchdog"
        vm.send("cat /proc/watchdog\n")
        vm.expect(r"feeder:   the kernel \(until user space opens /dev/watchdog\)", timeout=20)


@test
def reliability_wdt_magic_close_and_controls():
    """the Linux protocol: SIGTERM makes the feeder close with the magic 'V' (the watchdog stops; init does not restart it); "start" in
    /proc/watchdog arms it again, fed by the kernel; "echo V > /dev/watchdog" stops it again; a close without the 'V' leaves it running
    and nobody feeding it, so the machine restarts"""
    with Qemu(cmdline="text user=root wdt=4", name="t-rel-wdt-protocol") as vm:
        vm.expect(ROOT, timeout=120)
        wait_feeder(vm)
        root(vm, f"kill -TERM {feeder_pid(vm)}")
        vm.expect(r"magic close: the watchdog is stopped", timeout=30)
        vm.expect(r"the watchdog feeder was stopped", timeout=30)
        assert "state:    stopped" in root(vm, "cat /proc/watchdog")[0]
        stays_alive(vm, 12, "the watchdog was stopped by the magic close")
        out, rc = root(vm, "echo start 4 > /proc/watchdog; cat /proc/watchdog")
        assert rc == 0 and "state:    armed, 4 s" in out and "feeder:   the kernel" in out, out
        stays_alive(vm, 12, "the kernel feeds it after 'start'")
        mark = len(vm.text())
        out, rc = root(vm, "echo V > /dev/watchdog; cat /proc/watchdog")
        assert "state:    stopped" in out, out
        assert "magic close: the watchdog is stopped" in vm.text()[mark:], vm.text()[mark:]
        out, rc = root(vm, "echo start 4 > /proc/watchdog; echo still-armed")
        assert "still-armed" in out
        stays_alive(vm, 8, "armed again, fed by the kernel")
        vm.send("echo . > /dev/watchdog\n")                        # opened, fed once, closed without the 'V'
        vm.expect(r"/dev/watchdog was closed without the magic 'V': the watchdog keeps running", timeout=20)
        assert vm.wait_exit(60) is not None, "nobody fed the watchdog, but the machine did not restart"


@test
def reliability_wdt_blocked_reset_is_reported():
    """a chipset that keeps the reset back (QEMU's NO_REBOOT strap, -global ICH9-LPC.noreboot=true; on a PC the firmware may lock the bit):
    "test" stops all feeding, the timer runs out twice, the machine goes on running, and the kernel says that the reset is blocked"""
    with Qemu(cmdline="text user=root wdt=4", name="t-rel-wdt-blocked", extra=["-global", "ICH9-LPC.noreboot=true"]) as vm:
        vm.expect(ROOT, timeout=120)
        wait_feeder(vm)
        out, rc = root(vm, "echo test > /proc/watchdog; cat /proc/watchdog")
        assert rc == 0 and "nobody: test" in out, out
        vm.expect(r"watchdog: the timer ran out twice and the machine did not restart: the reset is blocked", timeout=60)
        assert vm.alive()
        out, _ = root(vm, "cat /proc/watchdog")
        assert "reset:    BLOCKED: the timer ran out twice and the machine went on running" in out, out


@test
def reliability_wdt_shutdown_gets_time():
    """a restart that was asked for (reboot) takes the watchdog away from its feeder and gives the shutdown 300 s, so that a slow shutdown is not
    cut short by a 4 s timeout"""
    with Qemu(cmdline="text user=root wdt=4", name="t-rel-wdt-shutdown") as vm:
        vm.expect(ROOT, timeout=120)
        wait_feeder(vm)
        vm.send("reboot\n")
        vm.expect(r"shutting down: the watchdog gives the shutdown 300 s and nobody feeds it any more", timeout=30)
        assert vm.wait_exit(60) is not None


@test
def reliability_wdt_off():
    """wdt=off: the loader leaves the timer stopped, the kernel finds and reports the hardware but arms nothing, there is no /dev/watchdog and
    no feeder; persist=off: no panic record region either, and the kernel says why"""
    with Qemu(cmdline="text user=root wdt=off persist=off", name="t-rel-off") as vm:
        vm.expect(ROOT, timeout=120)
        log = vm.text()
        assert "loader: wdt=off: the watchdog is not armed" in log and "loader: persist=off: no panic record region" in log, log[-3000:]
        out, _ = root(vm, "cat /proc/watchdog; ls /dev/watchdog; ps")
        assert "state:    off (wdt=off)" in out and "loader:   did not arm it (wdt=off)" in out and "No such file" in out and "wdt-feeder" not in out, out
        out, _ = root(vm, "dmesg | grep -E 'Momiji|Akyuu'; cat /proc/panic")
        assert "wdt=off: the watchdog is not armed" in out and "no panic record region (persist=off)" in out and "record:   none (persist=off)" in out, out


# ---------------------------------------------------------------- the record of how a start ended


@test
def reliability_record_watchdog_reset_and_clean_restart():
    """across real resets (no -no-reboot): a hung user space ends in a watchdog reset, and the next start says so twice: the TCO's own status
    ("the previous start ended in a watchdog reset") and the record in RAM (no shutdown, no panic, the last sign of life); a reboot that was
    asked for is recorded as such"""
    with Qemu(cmdline="text user=root wdt=6", name="t-rel-record", no_reboot=False) as vm:
        vm.expect(ROOT, timeout=120)
        assert "holds no record of the previous start" in vm.text()
        wait_feeder(vm)
        vm.send(f"kill -STOP {feeder_pid(vm)}\n")
        vm.expect(r"\[Momiji\] watchdog: the previous start ended in a watchdog reset", timeout=120)
        vm.expect(r"\[Akyuu\] the previous start \(#1, kernel built [^)]*\) ended without a shutdown or a panic: its last sign of life came \d+ s after it began",
                  timeout=60)
        vm.expect(ROOT, timeout=120)
        out, _ = root(vm, "cat /proc/watchdog; cat /proc/panic")
        assert "previous: the previous start ended in a watchdog reset" in out and "this start is #2" in out, out
        assert "previous: start #1 ended without a shutdown or a panic" in out, out
        wait_feeder(vm)
        vm.send("reboot\n")
        vm.expect(r"loader: entering kernel", timeout=120)
        vm.expect(r"\[Akyuu\] the previous start \(#2, kernel built [^)]*\) ended with a restart or poweroff that was asked for", timeout=60)
        vm.expect(ROOT, timeout=120)
        assert "ended in a watchdog reset" not in vm.text()[vm.text().rindex("loader: entering kernel"):], "a clean restart was taken for a watchdog reset"


@test
def reliability_panic_record_survives_the_restart():
    """a panic (written to /proc/panic) and a fatal exception (a kernel page fault: "pf") each restart the machine after panic=2 s, and the next
    start logs the record it found in RAM: "previous panic: ...", the panic's lines with the backtrace (and the registers of the exception);
    /proc/panic shows the end of that start's log; the watchdog, given the long budget before the panic's own restart, does not fire again"""
    with Qemu(cmdline="text user=root panic=2 wdt=10", name="t-rel-panic", no_reboot=False) as vm:
        vm.expect(ROOT, timeout=120)
        root(vm, "logger -t marker before-the-panic")
        vm.send("echo panic > /proc/panic\n")
        vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*", timeout=30)
        vm.expect(r"\[Akyuu\] the machine restarts in 2 s \(panic=2\)", timeout=30)
        vm.expect(r"\[Akyuu\] restarting now", timeout=30)
        vm.expect(r"\[Akyuu\] previous panic: requested through /proc/panic by 'gapsh' \(pid \d+\) \(start #1, ", timeout=120)
        vm.expect(r"\[Akyuu\]   \| backtrace:", timeout=30)
        vm.expect(r"\[Akyuu\]   \|   #0 [0-9a-f]+ put_proc\+0x", timeout=30)
        vm.expect(ROOT, timeout=120)
        second = vm.text()[vm.text().rindex("previous panic:"):]
        assert "ended in a watchdog reset" not in vm.text()[vm.text().rindex("restarting now"):], "the watchdog fired during the restart after the panic"
        out, _ = root(vm, "cat /proc/panic")
        assert "previous: start #1 ended in a kernel panic" in out and "--- the end of that start's log ---" in out, out
        assert "[marker] before-the-panic" in out and "*** INCIDENT: KERNEL PANIC ***" in out, "the record does not hold the end of the log:\n" + out
        vm.send("echo pf > /proc/panic\n")
        vm.expect(r"BOUNDARY VIOLATION: not-present page on write of 0xdead0000 by kernel", timeout=30)
        vm.expect(r"\[Akyuu\] previous panic: BOUNDARY VIOLATION: not-present page on write of 0xdead0000 by kernel \(start #2, ", timeout=120)
        vm.expect(r"\[Akyuu\]   \| rip=[0-9a-f]+ <put_proc\+0x", timeout=30)
        vm.expect(r"\[Akyuu\]   \| cr2=00000000dead0000", timeout=30)
        vm.expect(ROOT, timeout=120)
        assert "previous panic" in second


@test
def reliability_panic_reaches_the_log_file():
    """the start after a panic writes what the record held into its kernel log, and with it into TOUHOU.LOG on the HUMANVILLAGE stick: the
    panic is in the log file although the panicking start could not write it there itself (its block drivers sleep)"""
    from case_usbstorage import stick_image, stick, XHCI
    with stick_image("rel-panic-log") as img:
        with Qemu(cmdline="text user=root panic=2", name="t-rel-panic-log", extra=XHCI + stick(img), no_reboot=False) as vm:
            vm.expect(ROOT, timeout=180)
            assert "the kernel log is kept on the stick" in vm.text(), vm.text()[-2000:]
            vm.send("echo panic > /proc/panic\n")
            vm.expect(r"\[Akyuu\] previous panic: requested through /proc/panic", timeout=180)
            vm.expect(ROOT, timeout=180)
            deadline = time.time() + scaled(60)
            while True:
                log, _ = root(vm, "cat /HumanVillage/TOUHOU.LOG")
                if "previous panic: requested through /proc/panic" in log and "|   #0 " in log:
                    break
                assert time.time() < deadline, "the previous panic never reached TOUHOU.LOG:\n" + log[-2000:]
                time.sleep(2)


@test
def reliability_panic_on_trial_falls_back():
    """the case the record and panic= are for: an update on trial whose kernel panics at every start. The default on a trial start is a restart
    30 s after the panic, so start 1 and start 2 of the trial end by themselves, the loader counts them and starts the previous kernel, initrd
    and command line (update.fellback), and that start reports the panic of the trial start from the record in RAM"""
    esp = private_esp("t-rel-trial-panic-esp", extra=trial_files("text user=root"))
    with Qemu(cmdline="text user=root crash=panic", name="t-rel-trial-panic", esp=esp, no_reboot=False) as vm:
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=120)
        vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*", timeout=60)
        t0 = time.time()
        vm.expect(r"the machine restarts in 30 s \(panic=30: the default on a start under trial\)", timeout=30)
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=150)
        assert time.time() - t0 >= 25, f"the restart came {time.time() - t0:.0f} s after the panic, not 30"
        vm.expect(r"the machine restarts in 30 s", timeout=60)
        vm.expect(r"loader: starting the previous kernel and initrd \(kernel\.old, initrd\.old, cmdline\.old\): the new files did not confirm themselves in time",
                  timeout=150)
        vm.expect(r"\[Akyuu\] previous panic: requested panic \(start #\d+, on trial, \d+ s after it began", timeout=90)
        vm.expect(ROOT, timeout=120)
        out, _ = root(vm, "cat /proc/cmdline")
        assert "update.fellback" in out and "crash=panic" not in out, out
        out, _ = root(vm, "cat /proc/panic")
        assert "policy:   stay after a panic" in out and "previous: start #2 ended in a kernel panic" in out, out
        assert "update.trial=2" in out, "the record should hold the trial start's own log:\n" + out


@test
def reliability_wdt_hung_trial_kernel_falls_back():
    """the gap docs/UPDATE.md had: a pushed kernel that hangs at its first instruction. The loader arms the watchdog for it (it carries the
    feeder mark), the watchdog restarts the machine, twice, and the third start is the previous kernel: nobody presses the reset button"""
    esp = private_esp("t-rel-trial-hung-esp", kernel=hung_kernel(), extra=trial_files("text user=root"))
    with Qemu(cmdline="text user=root wdt=8", name="t-rel-trial-hung", esp=esp, no_reboot=False) as vm:
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=120)
        vm.expect(r"loader: the TCO watchdog is armed: 8 s", timeout=60)
        vm.expect(r"loader: entering kernel", timeout=30)
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=120)
        vm.expect(r"loader: the TCO watchdog is armed: 8 s", timeout=60)
        vm.expect(r"loader: starting the previous kernel and initrd \(kernel\.old, initrd\.old, cmdline\.old\)", timeout=120)
        vm.expect(r"\[Momiji\] watchdog: the previous start ended in a watchdog reset", timeout=90)
        vm.expect(ROOT, timeout=120)
        assert "update.fellback" in root(vm, "cat /proc/cmdline")[0]


@test
def reliability_record_region_elsewhere():
    """persist=ADDRESS moves the region; an address that is not free RAM in this start (the framebuffer here) gives no region, and both the
    loader and the kernel say why"""
    with Qemu(cmdline="text user=root persist=0x80000000", name="t-rel-region") as vm:
        vm.expect(ROOT, timeout=120)
        assert re.search(r"loader: no panic record region in this start: 0x0*80000000 is ", vm.text()), vm.text()[-3000:]
        out, _ = root(vm, "dmesg | grep Akyuu")
        assert "no panic record region (that address is not RAM on this machine)" in out, out
    with Qemu(cmdline="text user=root persist=0x5000000", name="t-rel-region2") as vm:
        vm.expect(ROOT, timeout=120)
        assert re.search(r"loader: panic record region 0x0*5000000 \(64 KiB, was free memory\)", vm.text()), vm.text()[-3000:]
        assert "record:   RAM at 0x5000000, 64 KiB" in root(vm, "cat /proc/panic")[0]


# ---------------------------------------------------------------- the netconsole


def netcons_boot(r, name, net="e1000", extra_cmdline="", target=None):
    target = target or f"10.0.2.2:{r.port}"
    return Qemu(cmdline=f"text user=root netconsole={target} {extra_cmdline}".strip(), name=name, net=net)


@test
def reliability_netcons_lines_and_panic():
    """netconsole=10.0.2.2:PORT on an e1000: the lines from before the network was up arrive (they waited in the log ring), then new ones as they
    are logged; a panic in user mode's way (root kills init) sends its text and backtrace through the card's polled transmit, interrupts off"""
    with Receiver(port=0, bind="127.0.0.1") as r, netcons_boot(r, "t-rel-netcons") as vm:
        vm.expect(ROOT, timeout=120)
        r.wait_for(r"^\[Gensokyo\] kernel 0\.1\.0 .* starting$", 60)
        r.wait_for(r"^\[Aya\] netconsole: the kernel log goes to 10\.0\.2\.2:", 30)
        r.wait_for(r"^\[Yukari\] eth0: lease 10\.0\.2\.15", 30)
        root(vm, "logger -t probe hello-netconsole")
        i, _ = r.wait_for(r"^\[probe\] hello-netconsole$", 20)
        out, _ = root(vm, "cat /proc/netconsole")
        assert "state:    sending" in out and "card:     eth0 (e1000)" in out and "polled transmit for a panic: yes" in out, out
        assert all("\x1b" not in t for t in r.texts()), "escape sequences reached the wire"
        vm.send("kill -9 1\n")
        r.wait_for(r"^\*\*\* INCIDENT: KERNEL PANIC \*\*\*$", 30, i)
        r.wait_for(r"^init \(pid 1\) exited with status ", 30, i)
        r.wait_for(r"^backtrace:$", 30, i)
        r.wait_for(r"^  #0 [0-9a-f]+ process_finalize\+0x", 30, i)
        r.wait_for(r"^\[Akyuu\] the machine stays like this; the watchdog restarts it within \d+ s", 30, i)
        vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC", timeout=30)


@test
def reliability_netcons_virtio_exception():
    """the polled path of virtio-net, with a fatal exception this time (a kernel page fault through /proc/panic): the trap frame's registers
    arrive too; the target is given in Linux's form (SPORT@/DEV,PORT@IP/)"""
    with Receiver(port=0, bind="127.0.0.1") as r, netcons_boot(r, "t-rel-netcons-virtio", net="virtio-net-pci", target=f"6600@/eth0,{r.port}@10.0.2.2/") as vm:
        vm.expect(ROOT, timeout=120)
        r.wait_for(r"^\[Yukari\] eth0: lease", 60)
        out = root(vm, "cat /proc/netconsole")[0]
        assert "card:     eth0 (virtio-net" in out and f"target:   10.0.2.2:{r.port} from port 6600 through eth0" in out, out
        n = len(r.lines())
        vm.send("echo pf > /proc/panic\n")
        r.wait_for(r"^BOUNDARY VIOLATION: not-present page on write of 0xdead0000 by kernel", 30, n)
        r.wait_for(r"^rip=[0-9a-f]+ <put_proc\+0x", 30, n)
        r.wait_for(r"^cr2=00000000dead0000 ", 30, n)
        r.wait_for(r"^backtrace:$", 30, n)


@test
def reliability_netcons_flood_and_no_loop():
    """a flood (20000 lines at once through the test hook in /proc/netconsole): the netconsole sends at most its rate, skips to the newest part
    and says how much it left out, the log stays usable and a line logged after the flood still arrives; the netconsole itself never writes
    to the kernel log (no loop): one line of its own at the start, and no more"""
    with Receiver(port=0, bind="127.0.0.1") as r, netcons_boot(r, "t-rel-netcons-flood", extra_cmdline="netconsole.rate=400") as vm:
        vm.expect(ROOT, timeout=120)
        r.wait_for(r"^\[Yukari\] eth0: lease", 60)
        t0 = time.time()
        out, rc = root(vm, "echo 'flood 20000' > /proc/netconsole; echo flood-done", timeout=300)
        assert rc == 0 and "flood-done" in out, out
        root(vm, "logger -t probe after-the-flood")
        r.wait_for(r"^\[probe\] after-the-flood$", 60)
        took = time.time() - t0
        texts = r.texts()
        flood = [t for t in texts if t.startswith("[flood] line ")]
        notes = [t for t in texts if t.startswith("-- netconsole:") and ("dropped" in t or "overwritten" in t)]
        assert notes, "the netconsole did not say that it left lines out:\n" + "\n".join(texts[-20:])
        assert 0 < len(flood) < 20000, len(flood)
        assert len(flood) <= 400 * took + 400 + 64, (len(flood), took)                 # the rate and one burst, at most
        out, _ = root(vm, "cat /proc/netconsole")
        assert re.search(r"dropped:  [1-9]\d* lines", out) or re.search(r"[1-9]\d* bytes overwritten", out), out
        own, _ = root(vm, "dmesg | grep 'Aya' | grep -v 'the kernel log goes to'; echo end-of-own-lines")
        assert own.strip() == "end-of-own-lines", "the netconsole wrote to the kernel log about itself:\n" + own
        assert not [t for t in texts if t.startswith("[Aya]") and "the kernel log goes to" not in t], "a line of the netconsole's own went into the log"


@test
def reliability_netcons_unreachable_is_quiet():
    """a target that never answers ARP (10.0.2.77 on QEMU's network): the netconsole waits and asks now and then, sends nothing it would lose,
    writes nothing to the kernel log about it, and the network goes on working for everything else"""
    with Qemu(cmdline="text user=root netconsole=10.0.2.77:6666", name="t-rel-netcons-unreachable", net="e1000") as vm:
        vm.expect(ROOT, timeout=120)
        time.sleep(scaled(15))
        out, _ = root(vm, "cat /proc/netconsole; dmesg | grep -c 'Aya'")
        assert "state:    waiting for 10.0.2.77 to answer ARP" in out and "sent:     0 datagrams" in out, out
        assert out.strip().endswith("1"), out
        out, rc = root(vm, "ping -c 1 10.0.2.2", timeout=60)
        assert rc == 0, out
