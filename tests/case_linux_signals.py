"""Real-time signals, who sent a signal, interval and POSIX timers, pthread_cancel and the set-user-id broadcast of the C library, under the Linux personality
(docs/COMPAT.md, "Signals, threads, timers").

Every test runs ordinary C programs (tests/linux/src, linked statically with glibc, built in WSL by tools/linux-oracle.py) in the guest and compares what they print,
line by line, with what the same binary printed on a real Linux; the stored outputs are in tests/linux/golden. The programs print only facts that do not depend on
timing (counts within a tolerance, orders, booleans), so one output is right for both systems.

    python tools/qemu-slot.py -- python tests/run_all.py linux_rt_
    LINUX_ONLY=glibc_rtsig python tools/qemu-slot.py -- python tests/run_all.py linux_rt_signals     (one program; LINUX_TRACE=1 logs every call)
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu
from case_linux import Inputs, TRACE, build_inputs, compare_with_linux, fetch, run

# name -> (the command line in the guest, its time limit in seconds)
PROGRAMS = {
    "glibc_rtsig": ("linux /tmp/glibc_rtsig", 300),
    "glibc_siginfo": ("linux /tmp/glibc_siginfo", 300),
    "glibc_timers": ("linux /tmp/glibc_timers", 300),
    "glibc_cancel": ("linux /tmp/glibc_cancel", 300),
    "glibc_setxid": ("linux /tmp/glibc_setxid", 300),
}

# a process that a signal killed is announced by the kernel on the console the program writes to
KERNEL_TAGS = {"glibc_rtsig": ("[Eiki]",), "glibc_siginfo": ("[Eiki]",)}
EXPECTED_KERNEL_LINES = {"glibc_rtsig": ("was exorcised by signal 44",), "glibc_siginfo": ("was exorcised by signal 15", "was exorcised by signal 9")}


def run_programs(names, vm_name):
    """Downloads the programs into a guest with a network card, runs each and compares its output with the stored output of a real Linux. LINUX_ONLY=a,b limits the
    programs (for debugging one of them); LINUX_LIMIT overrides the time limit; LINUX_TRACE=1 logs every call."""
    only = os.environ.get("LINUX_ONLY")
    names = [n for n in names if not only or n in only.split(",")]
    if not names:
        return
    build_inputs()
    failures = []
    with Inputs() as srv, Qemu(cmdline="text" + TRACE, name=vm_name, net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        for n in names:
            fetch(vm, srv.port, n)
        for n in names:
            cmd, limit = PROGRAMS[n]
            limit = int(os.environ.get("LINUX_LIMIT", limit))
            out, rc = run(vm, cmd, timeout=limit)
            try:
                kernel = compare_with_linux(n, out, rc, KERNEL_TAGS.get(n, ()))
                for what in EXPECTED_KERNEL_LINES.get(n, ()):
                    assert any(what in l for l in kernel), f"{n}: the kernel log lacks {what!r}; it had {kernel}"
            except AssertionError as e:
                failures.append(str(e))
    assert not failures, "\n\n".join(failures)


@test
def linux_rt_signals_match_linux():
    """Real-time signals 32..64 through glibc: sigqueue with a value, queued in order (lowest number first, oldest first), the handlers' siginfo (si_code, si_pid,
    si_uid, si_value), kill() queueing real-time but not standard signals, sigwaitinfo and sigtimedwait, SA_NODEFER, SA_RESETHAND, SA_RESTART, siglongjmp out of a
    handler, sigsuspend, a signal for the one thread that does not block it, 64-signal masks, the default action and SIG_IGN, and the errors of kill and
    rt_sigqueueinfo: the same lines as on a real Linux"""
    run_programs(["glibc_rtsig"], "t-linux-rtsig")


@test
def linux_rt_siginfo_matches_linux():
    """Who sent a signal: the siginfo of kill (SI_USER and the sender's pid and uid), raise, tgkill, tkill and pthread_kill (SI_TKILL), sigqueue, alarm (SI_KERNEL)
    and a broken pipe; SIGCHLD with si_code, si_pid, si_uid and si_status for a child that exits, is killed, stops and continues; waitid with P_PID, P_PGID, P_ALL,
    WNOWAIT, WNOHANG, WSTOPPED and WCONTINUED and its errors; kill(0, sig) and kill(-pgid, sig): the same lines as on a real Linux"""
    run_programs(["glibc_siginfo"], "t-linux-siginfo")


@test
def linux_rt_timers_match_linux():
    """Timers and clocks: setitimer and getitimer for real, virtual and profiling time, alarm, POSIX timers with a signal, a signal for one thread and the C library's
    thread notification, expiry counts, overruns, absolute times, timer_gettime/timer_settime/timer_delete, the clocks (CPU time of the process and of a thread
    included), clock_getres, nanosleep and clock_nanosleep with the errors Linux gives: the same lines as on a real Linux"""
    run_programs(["glibc_timers"], "t-linux-timers")


@test
def linux_rt_cancel_and_setxid_match_linux():
    """pthread_cancel of threads in read, nanosleep, a condition variable, a semaphore and pthread_join, cleanup handlers, disabled and asynchronous cancellation,
    pthread_exit; setuid and its relatives with other threads running (the C library's set-id broadcast): the same lines as on a real Linux"""
    run_programs(["glibc_cancel", "glibc_setxid"], "t-linux-cancel")
