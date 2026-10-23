"""Thread tests: the user-space thread suite inside utest, the futex self test inside the kernel, and /proc."""
import re

from harness import test, Qemu

THREAD_SECTIONS = [
    "C11 threads.h: thrd, mtx, cnd, tss, call_once, thread_local",
    "create, join, return values",
    "many threads, one mutex, one atomic",
    "/proc shows the threads",
    "producer/consumer with condition variables",
    "rwlock",
    "barrier",
    "errno, thread-specific data, once",
    "_Thread_local: initialised and zeroed copies per thread",
    "detached threads, 200 created and joined",
    "signals: pthread_kill, per-thread masks, process-directed delivery",
    "sigsuspend, sigwait, sigtimedwait and sigpending",
    "pthread_cancel: blocked read, condition wait, sleep, disabled state",
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


def run_utest(name):
    """Boots `utest` and returns the serial text once the verdict is in. The guest powers the machine off right after the
    verdict, and on a busy host QEMU can vanish before the harness has read the last serial bytes (the TCP stream is cut);
    a run that ends without a verdict is repeated (up to three times), a run with a verdict is never repeated."""
    last = None
    for attempt in range(3):
        with Qemu(cmdline="utest", name=name) as vm:
            try:
                vm.expect(r"UTEST (PASS|FAIL)", timeout=240)
            except RuntimeError as e:
                if not str(e).startswith("QEMU exited"):
                    raise
                last = e
                continue
            rc = vm.wait_exit(10)
            return vm.text(), rc
    raise last


@test
def threads_utest():
    """Threads: pthreads, futex, signals, fork/exec/exit from threads, limits and a leak check (utest section)."""
    text, rc = run_utest("t-threads-utest")
    for section in THREAD_SECTIONS:
        assert f"utest: threads: {section}" in text, f"thread section did not run: {section}\n{text[-1500:]}"
    assert "UTEST PASS" in text, text[-2500:]
    m = re.search(r"stress: free memory (\d+) KiB before, (\d+) KiB after", text)
    assert m, "the stress test did not report its page counts"
    assert int(m.group(2)) + 1024 >= int(m.group(1)), f"pages leaked: {m.group(0)}"
    assert rc == 0, f"utest exit status {rc}"


@test
def threads_selftest():
    """Kernel self test: futex wait queues (wake order, requeue, timeouts) and the thread bookkeeping."""
    with Qemu(cmdline="selftest", name="t-threads-selftest") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=90)
        text = vm.text()
        assert "futex" in text, "the futex self test did not run:\n" + text[-1200:]
        assert "SELFTEST PASS" in text, text[-1500:]
        assert vm.wait_exit(10) == 0
