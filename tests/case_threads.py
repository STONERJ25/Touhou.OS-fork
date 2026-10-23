"""Thread tests: the user-space thread suite inside utest, the futex self test inside the kernel, and /proc."""
import re

from harness import test, Qemu

THREAD_SECTIONS = [
    "create, join, return values",
    "many threads, one mutex, one atomic",
    "/proc shows the threads",
    "producer/consumer with condition variables",
    "rwlock",
    "barrier",
    "errno, thread-specific data, once",
    "detached threads, 200 created and joined",
    "signals: pthread_kill, per-thread masks, process-directed delivery",
    "futex, condition variable, semaphore and mutex timeouts",
    "thread limit: creation fails cleanly with EAGAIN",
    "SIGSEGV in a thread ends the whole process",
    "fork from a thread",
    "exec from a thread",
    "exit while other threads block in read, poll, futex, sleep and a condition variable",
    "exit_group from a thread other than the main one",
    "the last thread to leave ends the process with status 0",
    "stress: threads + pipes + mmap/munmap + fork, no pages leaked",
]


@test
def threads_utest():
    """Threads: pthreads, futex, signals, fork/exec/exit from threads, limits and a leak check (utest section)."""
    with Qemu(cmdline="utest", name="t-threads-utest") as vm:
        vm.expect(r"UTEST (PASS|FAIL)", timeout=240)
        text = vm.text()
        for section in THREAD_SECTIONS:
            assert f"utest: threads: {section}" in text, f"thread section did not run: {section}\n{text[-1500:]}"
        assert "FAIL " not in text.split("UTEST")[0] or "UTEST PASS" in text, text[-2500:]
        assert "UTEST PASS" in text, text[-2500:]
        m = re.search(r"stress: free memory (\d+) KiB before, (\d+) KiB after", text)
        assert m, "the stress test did not report its page counts"
        assert int(m.group(2)) + 1024 >= int(m.group(1)), f"pages leaked: {m.group(0)}"
        assert vm.wait_exit(10) == 0


@test
def threads_selftest():
    """Kernel self test: futex wait queues (wake order, requeue, timeouts) and the thread bookkeeping."""
    with Qemu(cmdline="selftest", name="t-threads-selftest") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=90)
        text = vm.text()
        assert "futex" in text, "the futex self test did not run:\n" + text[-1200:]
        assert "SELFTEST PASS" in text, text[-1500:]
        assert vm.wait_exit(10) == 0
