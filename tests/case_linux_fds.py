"""More than 64 descriptors under the Linux personality (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd").

Ordinary C programs (tests/linux/src/fds_*.c, linked statically with glibc, built in WSL by tools/linux-oracle.py) run in the guest, and what they print is compared
line by line with what the same binary printed on a real Linux (tests/linux/golden). They print only facts the manual pages promise.

    python tools/qemu-slot.py -- python tests/run_all.py linux_fds
    LINUX_ONLY=fds_many ... (one program); LINUX_LIMIT=600 (seconds per program); LINUX_TRACE=1 logs every call
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
    "fds_many": ("linux /tmp/fds_many", 300),
    "fds_fifo": ("linux /tmp/fds_fifo", 300),
    "fds_signalfd": ("linux /tmp/fds_signalfd", 300),
    "fds_pidfd": ("linux /tmp/fds_pidfd", 300),
    "fds_more": ("linux /tmp/fds_more", 300),
    "fds_stress": ("linux /tmp/fds_stress", 600),
}

# a process that a signal killed is announced by the kernel on the console the program writes to (as in case_linux_signals.py)
KERNEL_TAGS = {"fds_pidfd": ("[Eiki]",), "fds_more": ("[Eiki]",)}
EXPECTED_KERNEL_LINES = {"fds_pidfd": ("was exorcised by signal 15",)}


def kstats(vm):
    """/proc/kstats as a dict (kernel/warn.c): the kernel's own count of files, pipes, vnodes, processes, free pages ..."""
    out, _ = run(vm, "cat /proc/kstats")
    return {k: int(v) for k, v in (l.split() for l in out.splitlines() if len(l.split()) == 2 and l.split()[1].isdigit())}


# what a program may leave behind (docs/HARDENING.md, "objects that do not come back"): nothing of these
LEAK_KEYS = ("files", "pipes", "vnodes", "processes", "tasks", "user_threads", "kstacks", "unix_sockets", "ptys")
LEAK_CHECKED = ("fds_stress", "fds_more")


def run_programs(names, vm_name):
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
            if n in LEAK_CHECKED:
                run(vm, cmd, timeout=int(os.environ.get("LINUX_LIMIT", limit)))       # once to warm up (slabs, the tmpfs file's cache), then counted
                before = kstats(vm)
            out, rc = run(vm, cmd, timeout=int(os.environ.get("LINUX_LIMIT", limit)))
            try:
                if n in LEAK_CHECKED:
                    after = kstats(vm)
                    lost = {k: (before[k], after[k]) for k in LEAK_KEYS if after[k] != before[k]}
                    assert not lost, f"{n}: kernel objects did not come back (before, after): {lost}"
                    assert before["free_pages"] - after["free_pages"] <= 128, f"{n}: free pages went from {before['free_pages']} to {after['free_pages']}"
                    assert after["warnings"] == before["warnings"], f"{n}: the kernel warned: {after['warnings'] - before['warnings']} new"
                kernel = compare_with_linux(n, out, rc, KERNEL_TAGS.get(n, ()))
                for what in EXPECTED_KERNEL_LINES.get(n, ()):
                    assert any(what in l for l in kernel), f"{n}: the kernel log lacks {what!r}; it had {kernel}"
            except AssertionError as e:
                failures.append(str(e))
    assert not failures, "\n\n".join(failures)


@test
def linux_fds_many():
    """RLIMIT_NOFILE 1024/4096, a table filled with pipes and duplicates up to EMFILE, dup2 and F_DUPFD beyond the soft limit (EBADF, EINVAL), poll's nfds bound,
    raising the soft limit, /proc/self/limits, dup2/dup3/F_DUPFD_CLOEXEC to 70..2000, select on descriptor 1000, fork and execve with close-on-exec flags above 64,
    /proc/self/fd and close_range: the same output as on a real Linux"""
    run_programs(["fds_many"], "t-linux-fds-many")


@test
def linux_fds_fifo():
    """named pipes on tmpfs: mkfifo and mknod(S_IFIFO), the open rules of fifo(7) (blocking opens meet, O_RDWR never waits, O_NONBLOCK: reading opens at once, writing
    without a reader is ENXIO), end of file, SIGPIPE/EPIPE, poll/select/epoll with POLLHUP and POLLERR, fstat, lseek's ESPIPE, FIONREAD, data gone with the last opener,
    unlink while open, a ping-pong between two processes: the same output as on a real Linux"""
    run_programs(["fds_fifo"], "t-linux-fds-fifo")


@test
def linux_fds_signalfd():
    """signalfd and signalfd4: blocked signals read as 128-byte records (EAGAIN, EINVAL for a short buffer), raise, kill and sigqueue from this process and from a child,
    SIGCHLD with the child's status, four records in one read, a replaced mask, poll and epoll readiness, SFD_NONBLOCK and SFD_CLOEXEC, bad arguments, SIGKILL and
    SIGSTOP ignored in the mask: the same output as on a real Linux"""
    run_programs(["fds_signalfd"], "t-linux-fds-signalfd")


@test
def linux_fds_pidfd():
    """pidfd_open (EINVAL, ESRCH, PIDFD_NONBLOCK, close-on-exec), poll readiness when the process has exited and after the reap, pidfd_send_signal (signal 0, SIGTERM,
    a siginfo to ourselves, ESRCH after the reap, EBADF), waitid(P_PIDFD) with the child's code and status, /proc/self/fdinfo's Pid line: the same output as on a real Linux"""
    run_programs(["fds_pidfd"], "t-linux-fds-pidfd")


@test
def linux_fds_review():
    """the review round (docs/FDS_REVIEW.md), one boot. fds_more: pidfd_send_signal to a zombie is 0 and ESRCH after the reap, pidfd_getfd (a duplicate of another
    process's descriptor, close-on-exec; EINVAL, EBADF, ESRCH), a FIFO reader polls no POLLHUP before the first writer, POLLHUP after it left and none again with a
    second writer, an O_RDWR FIFO polled for writing is woken when a reader makes room, poll on a signalfd and on a pidfd is woken by the signal and the exit.
    fds_stress: four threads push a fresh process's descriptor table from 64 to 4096 slots at once (dup2, F_DUPFD_CLOEXEC, F_SETFD/F_GETFD, pipes, close) while the
    main thread forks children that use an inherited descriptor, six processes in a row. Both: the same output as on a real Linux, and the kernel's counts of files,
    pipes, vnodes, processes, threads and stacks back where they were before the (second) run, no warning in the kernel log"""
    run_programs(["fds_more", "fds_stress"], "t-linux-fds-review")


@test
def linux_fds_native_mkfifo():
    """the native mknod call and the `mkfifo` command: a named pipe in /tmp is marked | by ls -F, a background `cat` and an `echo` into it meet (the shell's own redirections
    open both ends) and the line comes out; a second mkfifo of the same name fails (EEXIST); no pipe is left in the kernel afterwards"""
    with Qemu(cmdline="text", name="t-linux-fds-mkfifo") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        before = kstats(vm)
        out, rc = run(vm, "mkfifo /tmp/f && ls -F /tmp/f")
        assert rc == 0 and "|" in out, f"mkfifo /tmp/f, then ls -F marks a FIFO with |: {rc} {out!r}"
        out, rc = run(vm, "mkfifo /tmp/f")
        assert rc != 0 and "exists" in out.lower(), f"a second mkfifo: {rc} {out!r}"
        out, rc = run(vm, "cat /tmp/f & echo hi > /tmp/f; wait", timeout=60)
        assert "hi" in out.split(), f"cat f & echo hi > f; wait: {rc} {out!r}"
        run(vm, "rm /tmp/f")
        after = kstats(vm)
        assert after["pipes"] == before["pipes"], f"pipes alive went from {before['pipes']} to {after['pipes']}"
