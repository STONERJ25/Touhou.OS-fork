"""Copy-on-write fork (docs/COW.md). The guest programs are user/test/cow_test.c and user/test/forkbench.c (/usr/lib/kernel-tests).
Run with `python tools/qemu-slot.py -- python tests/run_all.py cow_`.

cow_test's checks of what each side sees pass with the eager copy too (cow_off_gives_the_same_results runs them that way): they fail on a copy-on-write that shares too
much. What fails WITHOUT copy-on-write is the cost: a living child of a 32 MiB process takes a few hundred kB instead of 32 MiB (cow_fork_shares_pages), and a process
that holds 60% of the free memory can fork at all (cow_fork_under_memory_pressure)."""
import re

import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT
TESTS_DIR = "/usr/lib/kernel-tests"


def guest_program(vm, cmd, timeout=240):
    vm.send(f"{cmd}; echo __DO''NE__$?\n")
    m = vm.expect(r"__DONE__(\d+)\n", timeout=timeout)
    return vm.text(), int(m.group(1))


def kstats(vm):
    out, st = test_shell.run(vm, "cat /proc/kstats")
    assert st == 0, out
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^(\w+) (\d+)$", out, re.M)}


def run_cow_test(cmdline, name):
    with Qemu(cmdline=cmdline, mem="256M", name=name) as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/cow_test", timeout=300)
        tail = out.split("COW isolate_data", 1)[-1]
        assert rc == 0 and "COWTEST PASS" in tail and " FAIL" not in tail, out[-3000:]
        for check in ("isolate_bss", "isolate_heap", "isolate_stack", "isolate_mmap", "parent_writes_first", "chain", "many_children_one_page", "kernel_writes",
                      "threads_and_futex", "map_shared", "mprotect_munmap", "fork_exec_100", "leak_200_forks", "share_cost"):
            assert f"COW {check} ok" in out, (check, out[-3000:])
        vm.expect(PROMPT, timeout=30)
        after = kstats(vm)
        assert after["warnings"] == 0, after
        assert after["processes"] == before["processes"], (before, after)
        assert abs(after["free_pages"] - before["free_pages"]) < 300, (before["free_pages"], after["free_pages"])       # nothing leaked, nothing freed twice
        return int(re.search(r"COW share_kb=(-?\d+)", out).group(1))


@test
def cow_fork_shares_pages():
    """cow_test on the default kernel: every check passes, and a living child of a process with 32 MiB of touched memory costs less than 1 MiB of free memory."""
    share_kb = run_cow_test("text", "t-cow-on")
    assert 0 <= share_kb < 1024, share_kb


@test
def cow_off_gives_the_same_results():
    """cow=off: the same checks pass with the eager copy, and the same child costs the whole 32 MiB."""
    share_kb = run_cow_test("text cow=off", "t-cow-off")
    assert share_kb >= 32 * 1024, share_kb


@test
def cow_fork_under_memory_pressure():
    """A process with 60% of the free memory forks (impossible for an eager copy) and the child writes to every page: the copies run out of memory, the OOM killer ends one of the two, the system lives and the pages come back."""
    with Qemu(cmdline="text", mem="128M", name="t-cow-pressure") as vm:
        vm.expect(PROMPT, timeout=90)
        before = kstats(vm)
        out, rc = guest_program(vm, f"{TESTS_DIR}/cow_test pressure", timeout=300)
        assert "COW pressure forked" in out, out[-2000:]
        assert re.search(r"out of memory: killed 'cow_test' \(pid \d+\)", out), out[-2000:]
        assert "KERNEL PANIC" not in out, out[-2000:]
        vm.send("\n")
        vm.expect(PROMPT, timeout=60)
        test_shell.run(vm, "sleep 1")
        after = kstats(vm)
        assert after["oom_kills"] >= before["oom_kills"] + 1 and after["warnings"] == 0, (before, after)
        assert after["processes"] == before["processes"], (before, after)
        assert abs(after["free_pages"] - before["free_pages"]) < 600, (before["free_pages"], after["free_pages"])
        out, st = test_shell.run(vm, "echo alive")
        assert st == 0 and "alive" in out


def forkbench(cmdline, name):
    with Qemu(cmdline=cmdline, mem="512M", name=name) as vm:
        vm.expect(PROMPT, timeout=90)
        out, rc = guest_program(vm, f"{TESTS_DIR}/forkbench", timeout=600)
        assert rc == 0 and "FORKBENCH done" in out, out[-2000:]
        nums = {m.group(1): int(m.group(2)) for m in re.finditer(r"FORKBENCH (\w+)=(-?\d+)", out)}
        print(f"    forkbench [{cmdline}]: " + ", ".join(f"{k}={v}" for k, v in nums.items()))
        return nums


@test
def cow_forkbench_is_cheaper():
    """forkbench with and without cow=off (the numbers of docs/COW.md): the fork of a 64 MiB process is at least twice as fast and no fork fails."""
    off = forkbench("text cow=off", "t-cow-bench-off")
    on = forkbench("text", "t-cow-bench-on")
    assert on["fork_exec_bad"] == 0 and on["failed_64mib"] == 0, on
    assert on["child_cost_64mib_kb"] < 1024 <= off["child_cost_64mib_kb"], (on, off)
    assert on["fork_wait_64mib_us"] * 2 < off["fork_wait_64mib_us"], (on, off)
