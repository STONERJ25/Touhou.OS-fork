"""Kernel hardening (docs/HARDENING.md): execve limits, the system call fuzzer, resource exhaustion, SMEP/SMAP, the lockup
detector, panic screens. The guest programs live in user/test/ and are installed in /usr/lib/kernel-tests (not in anybody's PATH)."""
import re

import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT
TESTS_DIR = "/usr/lib/kernel-tests"


def guest_program(vm, cmd, timeout=240):
    """Runs a long guest command at the shell; returns (output, exit status). The marker is split so the echo does not match it."""
    vm.send(f"{cmd}; echo __DO''NE__$?\n")
    m = vm.expect(r"__DONE__(\d+)\n", timeout=timeout)
    return vm.text(), int(m.group(1))


# ------------------------------------------------------------------------------------------------ execve

# (command, substring that must appear in the output, expected exit status or None)
EXEC_SHELL = [
    ("mkdir -p /tmp/exd && cd /tmp/exd && seq 1 3000 | xargs touch && ls | wc -l", "3000", 0),
    ("cd /tmp/exd && ls * | wc -l", "3000", 0),
    ("cd /tmp/exd && /bin/ls -l * | wc -l", "3000", 0),
    ("cd /tmp/exd && rm * && ls | wc -l", "0", 0),
    ("seq 1 100000 | xargs echo | wc -w", "100000", 0),                         # batched into command lines that fit
    ("seq 1 100000 | xargs -n 30000 echo | wc -l", "4", 0),
    ("gapsh -c 'echo $#' x $(seq 1 5000)", "5000", 0),
    ("gapsh -c 'echo $#' x $(seq 1 40000) 2>&1", "Argument list too long", None),   # over the 32768 entries: a clean error
    ("/bin/true $(seq 1 40000); echo rc=$?", "Argument list too long", None),
    ("cd /; rmdir /tmp/exd", "", 0),
]


@test
def hardening_exec():
    """execve takes 5000 short arguments, a 100 KiB argument, 32768 entries and 512 KiB in all; E2BIG for more; xargs, ls * and rm * cope."""
    with Qemu(cmdline="text", name="t-hardening-exec") as vm:
        vm.expect(PROMPT, timeout=60)
        out, rc = guest_program(vm, f"{TESTS_DIR}/exectest", timeout=300)
        tail = out.split("exectest: execve limits begin", 1)[-1]
        assert "EXECTEST PASS" in tail and rc == 0, tail[-3500:]
        vm.expect(PROMPT, timeout=30)
        failures = []
        for cmd, want, status in EXEC_SHELL:
            got, st = test_shell.run(vm, cmd, timeout=120)
            if want not in got or (status is not None and st != status):
                failures.append(f"$ {cmd}\n  want {want!r} rc={status}, got rc={st}:\n{got.strip()[-400:]}")
        assert not failures, f"{len(failures)} check(s) failed:\n" + "\n".join(failures[:6])


# ------------------------------------------------------------------------------------------------ the system call fuzzer

import os


def run_sysfuzz(name, config, extra="", mem="512M"):
    """Boots with `kerneltest=sysfuzz`: init runs the fuzzer as root and powers off with its verdict. Returns (guest exit status, serial text)."""
    secs = int(re.search(r"time:(\d+)", config).group(1))
    cmdline = f"text kerneltest=sysfuzz ktargs={config}" + (" " + extra if extra else "")
    with Qemu(cmdline=cmdline, mem=mem, name=name) as vm:
        try:
            vm.expect(r"SYSFUZZ (PASS|FAIL)|INCIDENT: KERNEL PANIC", timeout=secs + 150)
        except RuntimeError:
            pass                                    # the guest powered off before the last line reached the log: the exit status still tells
        rc = vm.wait_exit(30)
        return rc, vm.text()


def check_sysfuzz(rc, text):
    assert "INCIDENT: KERNEL PANIC" not in text, "the kernel panicked:\n" + text[-3000:]
    assert "[WARNING]" not in text, "the kernel warned:\n" + text[text.find("[WARNING]") - 200:][:3000]
    assert rc == 0, f"sysfuzz ended with status {rc}:\n" + text[-3000:]


# Not in the default suite: a run takes minutes and the fuzzer is meant to be run with new seeds now and then (docs/HARDENING.md,
# "How to resume"). Set TOUHOU_HARDENING_FUZZ=1 to include it.
if os.environ.get("TOUHOU_HARDENING_FUZZ"):
    @test
    def hardening_sysfuzz():
        """The system call fuzzer, three seeds as an ordinary user (one with heap poisoning): no panic, no hang, no warning, nothing leaked."""
        for seed, extra in ((1, ""), (2, "heapdebug"), (3, "")):
            rc, text = run_sysfuzz(f"t-sysfuzz-{seed}", f"seed:{seed},time:40,workers:2", extra)
            check_sysfuzz(rc, text)
