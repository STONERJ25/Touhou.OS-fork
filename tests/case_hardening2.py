"""Kernel hardening, second round (docs/HARDENING.md): SMAP, resource limits, the OOM killer, the lockup detector.
The guest programs live in user/test/ and are installed in /usr/lib/kernel-tests (not in anybody's PATH). Run with `python tests/run_all.py hardening2_`
(through tools/qemu-slot.py when other agents share the PC)."""
import re

import test_shell
from harness import test, TESTS, Qemu

PROMPT = test_shell.PROMPT
TESTS_DIR = "/usr/lib/kernel-tests"
LINUX_TESTS = "/ScarletDevilMansion/share/linux-tests"          # the freestanding Linux programs of user/linuxtest (tests/case_linux.py)


def guest_program(vm, cmd, timeout=240):
    """Runs a long guest command at the shell; returns (output, exit status). The marker is split so the echo does not match it."""
    vm.send(f"{cmd}; echo __DO''NE__$?\n")
    m = vm.expect(r"__DONE__(\d+)\n", timeout=timeout)
    return vm.text(), int(m.group(1))


def crash_case(name, cmdline, expected, doc, timeout=40):
    """A deliberate kernel fault (kernel/crash_hardening.c, kernel/main.c): the panic must name what happened."""
    def run():
        with Qemu(cmdline=cmdline, name=f"t-{name}") as vm:
            vm.expect(r"INCIDENT: KERNEL PANIC", timeout=timeout)
            vm.expect(r"backtrace:\n(.*\n){2}", timeout=10)
            text = vm.text()
            assert re.search(expected, text), f"expected /{expected}/ in:\n{text[-2000:]}"
    run.__name__ = name
    run.__doc__ = doc
    TESTS.append(run)


# ------------------------------------------------------------------------------------------------ SMAP and SMEP

@test
def hardening2_smap_on():
    """A normal start turns SMAP (and SMEP) on, says so in the log, and the system works: a shell, files, a fork."""
    with Qemu(cmdline="text", name="t-h2-smap-on") as vm:
        vm.expect(PROMPT, timeout=60)
        out, st = test_shell.run(vm, "dmesg | grep -i -E 'smap|smep'")
        assert st == 0 and "SMAP on" in out and "SMEP on" in out, out
        out, st = test_shell.run(vm, "echo hello > /tmp/h2 && cat /tmp/h2 && (echo forked) && ls / | wc -l")
        assert st == 0 and "hello" in out and "forked" in out, out


@test
def hardening2_smap_off():
    """smap=off leaves SMAP off (and says why) and nothing else changes: the same system works without a single STAC or CLAC."""
    with Qemu(cmdline="text smap=off", name="t-h2-smap-off") as vm:
        vm.expect(PROMPT, timeout=60)
        out, st = test_shell.run(vm, "dmesg | grep -i smap")
        assert st == 0 and "SMAP off: smap=off is on the command line" in out and "SMAP on" not in out, out
        out, st = test_shell.run(vm, "echo hello > /tmp/h2 && cat /tmp/h2")
        assert st == 0 and "hello" in out, out


crash_case("hardening2_crash_smap", "crash=smap", r"SMAP VIOLATION: the kernel read the user address 0x400000 outside the copy window",
           "crash=smap: the kernel reads a user page without the copy window: a panic that says SMAP and names the address")
crash_case("hardening2_crash_smep", "crash=smep", r"SMEP VIOLATION: the kernel tried to execute the user address 0x400000",
           "crash=smep: the kernel jumps to a user page: a panic that says SMEP and names the address")
crash_case("hardening2_crash_smap_window", "crash=smap-window", r"SMAP VIOLATION: the kernel read the user address 0x400000",
           "every copy routine (successful, faulting, clearing, string) closes the window: the plain load after them is still refused")


@test
def hardening2_ac_set_in_user_mode():
    """A program that sets EFLAGS.AC itself does not switch SMAP off for the kernel: ticks and system calls (also failing ones) arrive with AC set and no handler runs with it."""
    with Qemu(cmdline="text", name="t-h2-acflag") as vm:
        vm.expect(PROMPT, timeout=60)
        out, st = guest_program(vm, f"{TESTS_DIR}/acflag", timeout=120)
        tail = out.split("acflag:", 1)[-1]
        assert "ACFLAG PASS" in tail and st == 0, out[-1500:]
        vm.expect(PROMPT, timeout=30)
        out, st = test_shell.run(vm, "dmesg | grep -c WARNING; cat /proc/kstats | grep warnings")
        assert re.search(r"^0$", out, re.M) and re.search(r"warnings 0", out), out


# ------------------------------------------------------------------------------------------------ resource limits

def kstats(vm):
    """/proc/kstats as a dict of ints (free_pages, oom_kills, watchdog_reports, tmpfs_pages, processes, ...)."""
    out, st = test_shell.run(vm, "cat /proc/kstats")
    assert st == 0, out
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^(\w+) (\d+)$", out, re.M)}


def pids(vm):
    """{pid: command} from ps."""
    out, st = test_shell.run(vm, "ps")
    return {int(m.group(1)): m.group(2) for m in re.finditer(r"^\s*(\d+)\s+\S+\s+\S\s+\S+\s+(\S+)", out, re.M)}


def noisy_line(vm, shell_expr, tag, tries=4):
    """For a guest that logs a long report every few seconds (the lockup tests): runs `echo TAG $(shell_expr)` and returns what follows the tag on the one line the answer is.
    A report that lands inside the answer garbles that line, so it is asked again (the typed text contains the tag too, split by quotes so that it cannot match)."""
    last = None
    for _ in range(tries):
        vm.send(f"echo {tag[:2]}''{tag[2:]} $({shell_expr})\n")
        try:
            return vm.expect(rf"\n{tag} (.*?)\n", timeout=30).group(1).strip()
        except (TimeoutError, RuntimeError) as e:
            last = e
    raise last


ULIMIT_CHECKS = [
    # (command, substring that must appear in the output, exit status or None)
    ("ulimit -n; ulimit -Hn", "64\n64", 0),
    ("ulimit -u; ulimit -Hu", "128\n128", 0),
    ("ulimit -s; ulimit -c; ulimit -v", "8192\n0\nunlimited", 0),
    ("(ulimit -n 20; ulimit -n)", "20", 0),
    ("(ulimit -n 20; sh -c 'ulimit -n; ulimit -Hn')", "20\n20", 0),                          # inherited by what the shell starts
    ("(ulimit -v 65536; ulimit -v; ulimit -Hv)", "65536\n65536", 0),
    ("(ulimit -Sn 10; ulimit -n; ulimit -Hn)", "10\n64", 0),                                  # -S: only the soft limit
    ("(ulimit -Hn 30; ulimit -n; ulimit -Hn)", "30\n30", 0),                                  # lowering the hard limit pulls the soft one with it
    ("(ulimit -Hn 30; ulimit -Hn 40) 2>&1", "cannot modify limit: Operation not permitted", 1),   # a lowered hard limit stays lowered
    ("(ulimit -Hn 30; ulimit -Sn 40) 2>&1", "cannot modify limit: Invalid argument", 1),      # a soft limit above the hard one
    ("(ulimit -n hard; ulimit -n soft; ulimit -n 12) && echo ok", "ok", 0),
    ("ulimit -n abc 2>&1", "invalid number", 1),
    ("ulimit -z 2>&1", "invalid option", 2),
    ("ulimit -a | wc -l", "10", 0),
    ("(ulimit -n 6; cat /dev/null /dev/null /dev/null /dev/null 2>&1; echo rc=$?)", "rc=0", 0),
]


@test
def hardening2_limits():
    """rlimtest (get/set/prlimit and what each limit enforces: descriptors, processes, address space, data, stack) and the ulimit builtin."""
    with Qemu(cmdline="text", name="t-h2-limits") as vm:
        vm.expect(PROMPT, timeout=60)
        out, rc = guest_program(vm, f"{TESTS_DIR}/rlimtest", timeout=240)
        tail = out.split("rlimtest: resource limits begin", 1)[-1]
        assert "RLIMTEST PASS" in tail and rc == 0, tail[-3500:]
        vm.expect(PROMPT, timeout=30)
        failures = []
        for cmd, want, status in ULIMIT_CHECKS:
            got, st = test_shell.run(vm, cmd, timeout=60)
            if want not in got or (status is not None and st != status):
                failures.append(f"$ {cmd}\n  want {want!r} rc={status}, got rc={st}:\n{got.strip()[-300:]}")
        assert not failures, f"{len(failures)} ulimit check(s) failed:\n" + "\n".join(failures[:6])
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest fdleak")
        assert "OOMTEST PASS" in out and rc == 0, out[-1200:]
        vm.expect(PROMPT, timeout=30)
        out, rc = guest_program(vm, f"(ulimit -n 20; {TESTS_DIR}/oomtest fdleak)")                    # a descriptor leak ends at the limit, whatever it is
        assert "OOMTEST PASS" in out and re.search(r"fdleak: 1\d descriptors", out) and rc == 0, out[-1200:]
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        for key in ("files", "vnodes", "processes", "tasks", "warnings"):
            assert after[key] == before[key], f"{key} {before[key]} -> {after[key]}"


@test
def hardening2_fork_bomb_stops_at_the_limit():
    """A fork bomb ends with EAGAIN at the user's process limit (the default, and a lower one set with ulimit -u), everything is reaped, fork works again, nobody is killed."""
    with Qemu(cmdline="text", name="t-h2-forkbomb") as vm:
        vm.expect(PROMPT, timeout=60)
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest forkbomb 128", timeout=240)
        assert "OOMTEST PASS" in out and rc == 0, out[-1500:]
        vm.expect(PROMPT, timeout=30)
        out, rc = guest_program(vm, f"(ulimit -u 24; {TESTS_DIR}/oomtest forkbomb 24)", timeout=240)
        assert "OOMTEST PASS" in out and rc == 0, out[-1500:]
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] == 0 and after["warnings"] == 0, after
        assert after["processes"] == before["processes"] and after["tasks"] == before["tasks"] and after["kstacks"] == before["kstacks"], (before, after)
        assert abs(after["free_pages"] - before["free_pages"]) < 400, (before["free_pages"], after["free_pages"])


# ------------------------------------------------------------------------------------------------ the OOM killer

OOM_MEM = "128M"


def oom_boot(name, extra=""):
    return Qemu(cmdline=f"text sshd {extra}".strip(), mem=OOM_MEM, name=name)


@test
def hardening2_oom_kills_the_hog():
    """A program that maps and touches memory until it is stopped is ended by the OOM killer (log line, oom_kills), init, the shell and sshd live, and the pages come back."""
    with oom_boot("t-h2-oom-hog") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        procs_before = {pid: name for pid, name in pids(vm).items() if name in ("init", "sshd", "gapsh")}      # (ps itself and the programs it was started beside come and go)
        assert set(procs_before.values()) == {"init", "sshd", "gapsh"}, procs_before
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest hog 1024", timeout=240)
        assert rc == 128 + 9, (rc, out[-1500:])                                       # SIGKILL, as `kill -9` ends a program
        assert re.search(r"out of memory: killed 'oomtest' \(pid \d+\), \d+ pages", out), out[-1500:]
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] == before["oom_kills"] + 1, (before, after)
        assert after["warnings"] == 0, after
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])     # the hog's memory is back
        procs_after = pids(vm)
        for pid, name in procs_before.items():
            assert procs_after.get(pid) == name, f"{name} (pid {pid}) is gone: {procs_after}"
        out, st = test_shell.run(vm, "echo alive; cat /proc/oom")
        assert st == 0 and "alive" in out and "kills: 1" in out and "last: 'oomtest'" in out, out
        out, st = test_shell.run(vm, "dmesg | grep -c 'out of memory'")
        assert out.strip() == "1", out


@test
def hardening2_oom_worker_survives_a_slow_hog():
    """While one program eats memory slowly, another that needs fresh memory all the time (and so meets the shortage first) waits for the victim and finishes correctly."""
    with oom_boot("t-h2-oom-slow") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        vm.send(f"({TESTS_DIR}/oomtest hog 512 20 > /tmp/hog.out 2>&1; echo $? > /tmp/hog.rc) & echo HOG''PID=$!\n")       # (the shell forgets a job it has reaped: the subshell keeps the status)
        vm.expect(r"HOGPID=\d+", timeout=30)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest work 25", timeout=240)
        assert "OOMWORK PASS" in out and rc == 0, out[-2000:]
        vm.expect(PROMPT, timeout=30)
        out, st = test_shell.run(vm, "i=0; while [ ! -f /tmp/hog.rc ] && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done; cat /tmp/hog.rc", timeout=120)
        assert out.strip() == "137", out                                                # the hog was ended by SIGKILL; the worker was not
        after = kstats(vm)
        assert after["oom_kills"] == before["oom_kills"] + 1, (before, after)
        out, st = test_shell.run(vm, "dmesg | grep 'out of memory'")
        assert out.count("killed 'oomtest'") == 1, out                                  # one shortage, one victim: not the worker
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])


@test
def hardening2_oom_fork_of_a_big_process():
    """fork copies every page, so a big process that keeps forking runs the machine out of memory inside fork itself: the killer ends children (the youngest of equal size), the parent lives and gets all its children."""
    with oom_boot("t-h2-oom-forkfat") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        keep_before = {pid: name for pid, name in pids(vm).items() if name in ("init", "sshd", "gapsh")}
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest forkfat 16 24", timeout=300)
        assert "OOMTEST PASS" in out and rc == 0, out[-2000:]
        m = re.search(r"forkfat: 24 children made of 16 MiB each, (\d+) ended by the killer, (\d+) forks failed, the parent's memory is intact", out)
        assert m and int(m.group(1)) >= 1, out[-1500:]                                  # 24 x 16 MiB cannot fit in 128 MiB: some children had to go
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] >= before["oom_kills"] + 1 and after["warnings"] == 0, (before, after)
        for key in ("processes", "tasks", "kstacks", "files", "vnodes"):
            assert after[key] == before[key], f"{key} {before[key]} -> {after[key]}"
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])
        keep_after = {pid: name for pid, name in pids(vm).items() if name in ("init", "sshd", "gapsh")}
        assert keep_after == keep_before, (keep_before, keep_after)


@test
def hardening2_oom_kills_a_threaded_hog():
    """The victim is a process with four threads that all fault in fresh memory: every thread goes (the picked one, the ones that fault while it dies), and the kernel stacks, the thread count and the pages come back."""
    with oom_boot("t-h2-oom-threads") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest hogthreads 3", timeout=240)
        assert rc == 128 + 9, (rc, out[-1500:])
        assert re.search(r"out of memory: killed 'oomtest' \(pid \d+\), \d+ pages", out), out[-1500:]
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] == before["oom_kills"] + 1 and after["warnings"] == 0, (before, after)
        for key in ("processes", "tasks", "user_threads", "kstacks"):
            assert after[key] == before[key], f"{key} {before[key]} -> {after[key]}"
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])


@test
def hardening2_oom_kills_a_linux_shared_memory_hog():
    """The Linux personality gives shared anonymous memory all its pages at the call (kernel/linux/l3.c): a Linux program that asks for it until it is stopped is ended by the OOM killer, not left
    to eat the kernel's reserve and spin on ENOMEM; its pages come back."""
    with oom_boot("t-h2-oom-linux-shm") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        out, rc = guest_program(vm, f"linux {LINUX_TESTS}/lnx_shmhog", timeout=240)
        assert rc == 128 + 9, (rc, out[-1500:])
        assert re.search(r"out of memory: killed '\S+' \(pid \d+\), \d+ pages", out), out[-1500:]
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] == before["oom_kills"] + 1 and after["warnings"] == 0, (before, after)
        for key in ("processes", "tasks", "kstacks"):
            assert after[key] == before[key], f"{key} {before[key]} -> {after[key]}"
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])        # the shared pages are back


@test
def hardening2_oom_tmpfs_is_limited_as_a_whole():
    """A runaway write into /tmp ends with ENOSPC at half of RAM, nobody is killed, the machine answers, and deleting the file gives the pages back."""
    with oom_boot("t-h2-oom-tmpfs") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest tmpfill /tmp", timeout=300)
        assert "OOMTEST PASS" in out and rc == 0, out[-1500:]
        written = int(re.search(r"tmpfill: (\d+) MiB written", out).group(1))
        assert 8 <= written < 128 // 2, written                                           # more than a few MiB, less than half of the 128 MiB machine
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["oom_kills"] == 0 and after["warnings"] == 0, after
        assert abs(after["tmpfs_pages"] - before["tmpfs_pages"]) < 50 and abs(after["free_pages"] - before["free_pages"]) < 600, (before, after)
        out, st = test_shell.run(vm, "echo still-here; df /tmp | tail -1")
        assert st == 0 and "still-here" in out, out


@test
def hardening2_oom_off_leaves_a_failing_fault():
    """oom=off keeps the reserve and the failing allocation but kills nobody: the hog gets SIGSEGV at the wall, as before the killer existed."""
    with oom_boot("t-h2-oom-off", "oom=off") as vm:
        vm.expect(PROMPT, timeout=90)
        out, rc = guest_program(vm, f"{TESTS_DIR}/oomtest hog 1024", timeout=240)
        assert rc == 128 + 11, (rc, out[-1500:])
        assert "out of memory: killed" not in out and "Yuyuko is hungry" in out, out[-1500:]
        vm.expect(PROMPT, timeout=30)
        out, st = test_shell.run(vm, "cat /proc/oom")
        assert "killer: off" in out and "kills: 0" in out, out


# ------------------------------------------------------------------------------------------------ the lockup detector

@test
def hardening2_lockup_is_reported():
    """crash=lockup: a kernel thread that spins with interrupts on is reported after the threshold (lockup.thresh=3), with its name, registers and a backtrace; the machine goes on."""
    with Qemu(cmdline="text crash=lockup lockup.thresh=3", name="t-h2-lockup") as vm:
        m = vm.expect(r"\[Momiji\] soft lockup: '(\S+)' \(kernel thread, tid \d+\) has run in the kernel for 3 s", timeout=90)
        assert m.group(1) == "lockup", m.group(0)
        vm.expect(r"backtrace:\n[^\n]+\n", timeout=15)
        text = vm.text()
        assert re.search(r"rip=[0-9a-f]{16} <spin_forever\+", text), text[-2500:]               # the register state names the loop
        assert "spin_forever" in text.split("backtrace:", 1)[-1][:1500], text[-2500:]         # and so does the backtrace
        assert re.search(PROMPT, vm.text()), "the shell never came up"                          # (its prompt came before the first report: the boot finished, then the spinner was found)
        # The machine is not stopped: the shell answers, while the report repeats every 3 s (it prints from the timer tick, so a report can land inside an answer: ask again).
        answer = noisy_line(vm, "echo $((6*7))", "ALIVE")
        assert answer == "42", answer
        stats = noisy_line(vm, "cat /proc/kstats | tr '\\n' ' '", "KSTATS")
        k = {m.group(1): int(m.group(2)) for m in re.finditer(r"(\w+) (\d+)", stats)}
        assert k["watchdog_reports"] >= 1 and k["warnings"] == 0, k


@test
def hardening2_lockup_panics_with_panic_on_lockup():
    """With panic_on_lockup the report is a panic: the usual incident text with the task, the registers and the backtrace, and the restart that panic=2 asks for."""
    with Qemu(cmdline="text crash=lockup lockup.thresh=3 panic_on_lockup panic=2", name="t-h2-lockup-panic") as vm:
        vm.expect(r"INCIDENT: KERNEL PANIC", timeout=90)
        vm.expect(r"backtrace:\n[^\n]+\n", timeout=15)                                          # (not `(.*\n)+`: with re.S that swallows the lines that follow, "restarts in 2 s" among them)
        vm.expect(r"restarts in 2 s", timeout=30)
        text = vm.text()
        assert re.search(r"soft lockup: 'lockup' \(kernel thread, tid \d+\) has run in the kernel for 3 s .*\(panic_on_lockup\)", text), text[-2500:]
        assert "task: lockup" in text and "spin_forever" in text, text[-2500:]


@test
def hardening2_lockup_off():
    """lockup=off: the same spinning thread is never reported (and the start says the detector is off)."""
    with Qemu(cmdline="text crash=lockup lockup.thresh=2 lockup=off", name="t-h2-lockup-off") as vm:
        vm.expect(PROMPT, timeout=60)
        import time
        time.sleep(8)                                                                           # several thresholds' worth
        out, st = test_shell.run(vm, "dmesg | grep -c 'soft lockup:'; dmesg | grep 'soft lockup detector'")
        assert out.splitlines()[0].strip() == "0" and "detector: off (lockup=off)" in out, out
        assert kstats(vm)["watchdog_reports"] == 0


import contextlib


@contextlib.contextmanager
def lockup_watch(secs=3):
    """Every guest started inside the block gets `lockup.thresh=SECS panic_on_lockup`: a false positive of the detector in a long legitimate kernel job is then a panic."""
    import qemu
    original = qemu.Qemu._prepare

    def prepare(self):
        if "lockup" not in self.cmdline:
            self.cmdline = (self.cmdline + f" lockup.thresh={secs} panic_on_lockup").strip()
        return original(self)
    qemu.Qemu._prepare = prepare
    try:
        yield
    finally:
        qemu.Qemu._prepare = original


# Work that keeps the kernel busy on purpose, after the boot (the detector is armed from the end of the boot on): the repair of a damaged stick by the automount thread, a slow repair
# that outlasts the hardware watchdog's timer, a 60 MiB file and 200 files through the write-back cache with 96 MiB of RAM, the NTFS scan of a mounted volume, a big exec, the in-kernel
# self tests. (The whole damage menu and the power-cut campaigns of the boot-time repair run before the detector is armed; `python tests/run_all.py fsck_boot` has them.)
QUIET_TESTS = ("selftest", "hardening_exec", "fsck_boot_stick", "fsck_boot_watchdog", "storage_small_ram", "ntfs_mount_read_copy_and_refuse_every_write")


@test
def hardening2_lockup_detector_stays_quiet():
    """The long legitimate kernel jobs (a stick repaired at run time, a slow repair, a 60 MiB file through the cache in 96 MiB of RAM, an NTFS scan, a big exec, the self tests) run with a 3 s threshold and panic_on_lockup: no report."""
    import harness
    wanted = {fn.__name__: fn for fn in harness.TESTS if fn.__name__ in QUIET_TESTS}
    missing = [n for n in QUIET_TESTS if n not in wanted]
    assert not missing, f"tests that are gone: {missing}"
    failures = []
    with lockup_watch(3):
        for name in QUIET_TESTS:
            try:
                wanted[name]()
            except Exception as e:                                                              # noqa: BLE001
                failures.append(f"{name}: {str(e)[:600]}")
    assert not failures, "\n".join(failures)
