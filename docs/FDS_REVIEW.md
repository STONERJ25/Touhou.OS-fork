# Review of the descriptor table, FIFOs, signalfd and pidfd (2026-11-24)

A second reading of the work described in docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd" (branch `agent/linux-fds`), done on `agent/fds-2` the hour after it
was written. What was looked for: a table that is replaced while somebody still holds the old one, error paths that leak, FIFO behaviour against fifo(7) and a real
Linux, signalfd and pidfd details, and regressions for anonymous pipes. Severity is one of crash / leak / wrong result / cosmetic.

The facts the review rests on: the kernel runs on one CPU and **is preemptive** (kernel/sched/task.c: the timer switches tasks in kernel code too), "the table lock"
is `irq_save`, and threads of a process share one `Process` and so one table.

## Findings

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | `fd_reserve` freed the replaced table at once; lock-free readers and writers could touch freed memory | crash (heap corruption) | fixed |
| 2 | `fd_flags[fd] |= FD_CLOEXEC` and `F_SETFD` outside the lock can lose the flag when a sibling grows the table in the same instant | wrong result | fixed (second round) |
| 3 | `sys_dup2`, `sys_fcntl(F_DUPFD)` and others use `fd_get` and take the reference afterwards | crash (older than this work) | fixed for `dup`, `dup2`, `fcntl` (second round); the general pattern stays |
| 4 | signalfd and pidfd had no wait queue: poll, select, epoll looked again every few ms | cosmetic (latency, CPU) | fixed |
| 5 | `pidfd_send_signal` to an unreaped zombie answered `ESRCH`; Linux answers 0 | wrong result | fixed |
| 6 | `kill(2)` of an unreaped zombie answers `ESRCH`; Linux answers 0 | wrong result | open (kernel/sys/signal.c, owner's call) |
| 7 | A FIFO reader polled `POLLHUP` before any writer had ever opened; Linux does not | wrong result | fixed |
| 8 | A FIFO opened `O_RDWR` and polled for `POLLOUT` was not woken when a reader made room | wrong result (a hang until the time-out) | fixed |
| 9 | `pidfd_open` of a thread id answered `ESRCH`; Linux 6.18 answers `ENOENT` (the manual page: `EINVAL`, older kernels) | wrong result | fixed |
| 10 | `pidfd_getfd` (438) missing | missing | added |
| 11 | No native `mknod`, no `mkfifo` command | missing | added |
| 12 | Stale comment at `PQ_MAX_ENTRIES` ("the table is 64 entries long") | cosmetic | fixed |
| 13 | A table never shrinks, `exec` keeps a grown one; retired tables stay until exit | cosmetic (at most 72 KiB per process) | by design, documented |

### 1. The replaced table was freed while it could still be in use (fixed)

`fd_reserve` copied the table into a bigger block under the lock and then called `kfree` on the old one, with the comment that nothing outside the descriptor
functions keeps a pointer into the table across a sleep. No sleep is needed: `fd_get` (no lock), `F_GETFD`/`F_SETFD` in `sys_fcntl`, the `me()->fd_flags[fd] |=
FD_CLOEXEC` lines in kernel/linux (`syscall.c`, `net.c`, `fsops.c`, `ioctl.c`, `l3.c`), /proc's `fd` and `fdinfo` readers and the OOM killer's
`orphan_tmpfs_pages` all load `p->fds` or `p->fd_flags` into a register and index it with interrupts on. A timer interrupt between the load and the access, and a
sibling thread that grows the table before the first runs again, leaves a pointer into freed heap: a read returns whatever the allocator put there (a wild `File *`),
and the flag writers **write one byte into a block that may belong to somebody else by then**. The window is a few instructions, so it would show up as a rare,
unexplainable corruption in a threaded program that opens many files, which is exactly what the feature is for (Python with threads, a server).

Fix (kernel/sys/process.c, `fd_reserve`, `process_free`; `Process.fd_retired`): a replaced heap table is linked into a list of the process and freed in
`process_free`, when no thread of the process and no reader that found it through the process table can exist any more. A stale table is a correct snapshot of the
instant it was replaced, so a reader behaves as if it had run a moment earlier. The tables double, so the retired ones together are smaller than the live one:
36 KiB at most for a process that reached 4096 slots (72 KiB with the live table).

Test: `linux_fds_review` runs `tests/linux/src/fds_stress.c` (below). **There is no test that fails before this fix**: the window is a few instructions wide and
the tick is 1 ms, so the stress test exercises the paths and checks for leaks and mistakes but cannot be expected to hit the race (it was not run on the unfixed
kernel: there was no time for a second boot, and a pass there would have shown nothing). The argument above is from reading the code.
A deterministic test needs a hook that preempts between the load and the access (a kernel selftest with a forced `yield` in `fd_get`): not written.

### 2. A lost close-on-exec flag (fixed in the second round)

With finding 1 fixed, a writer that loaded the old `fd_flags` writes into the retired table and the flag is lost; and the read-modify-write `|=` is not atomic
against another thread's `F_SETFD` of the same descriptor either. `fd_setflags(p, fd, set, clear)` now does it under the lock (used in kernel/linux/l3.c). The other
sites (converted in the second round, see below) were: `sys_fcntl` `F_SETFD` (kernel/sys/syscalls.c), `lnx_pipe2` and `lnx_dup3` (kernel/linux/syscall.c:657, 667), `socket`
(kernel/linux/net.c:77), `close_range` (kernel/linux/fsops.c:169), `FIOCLEX` (kernel/linux/ioctl.c:73). Each is a one-line change to `fd_setflags`.

### 3. `fd_get` without a reference (older than this work; the duplicating calls fixed in the second round)

`sys_dup2` and `F_DUPFD` call `fd_get` and then `file_get`: a sibling thread that closes the descriptor in between frees the `File` first. Every system call that
uses `fd_get` and then sleeps in the file's operation has the same exposure; `fd_get_ref` exists for it. Not new with the growing table and not changed here.

### Second round (the same afternoon): findings 2 and 3, and who still touches the table directly

* Every writer of descriptor flags goes through `fd_setflags` (set and clear under the table lock, `-EBADF` for a descriptor that is not open): `F_SETFD`
  (kernel/sys/syscalls.c), `pipe2` and `dup3` (kernel/linux/syscall.c), `accept4` (kernel/linux/net.c), `close_range(CLOSE_RANGE_CLOEXEC)`
  (kernel/linux/fsops.c), `FIOCLEX`/`FIONCLEX` (kernel/linux/ioctl.c), `posix_openpt` (kernel/linux/l3.c). `F_GETFD` and exec's close-on-exec loop read them with
  the new `fd_getflags`.
* `sys_dup`, `sys_dup2` and the whole of `sys_fcntl` (so `F_DUPFD`, `F_GETFL`, `F_SETFL` too) take their file with `fd_get_ref` and put it away on every path
  that does not hand it to the table. `lnx_fcntl`'s `F_DUPFD_CLOEXEC` and `pidfd_send_signal`/`waitid(P_PIDFD)` did already.
* What still reads `p->fds` or `p->fd_flags` outside kernel/sys/process.c, and why that is right (`grep -rn "\->fds\b\|fd_flags\b" kernel`):
  kernel/fs/procfs.c (three sites) and kernel/linux/procfs.c (four sites, one of them the `fd_flags` read of `fdinfo`): all inside `irq_save`, together with the
  `process_by_pid` that found the process; kernel/mm/oom.c `orphan_tmpfs_pages`: `pick_victim`'s caller has interrupts off; kernel/linux/fsops.c
  `lnx_open_via_proc` and kernel/linux/epoll.c `pidfd_getfd`: inside `irq_save`, the reference taken there; `process_fork_ex`'s copy: inside `irq_save`;
  `process_finalize` and the two undo loops of a failed fork: the last thread of the process, or a child nobody else can see yet;
  kernel/drivers/blk_unlock.c `claimable`: **unlocked**, the caller's own table, compares pointers only and dereferences none (a stale answer is a grant refused or
  given to a file the process held an instant ago; the retired-table rule keeps the read inside valid memory).
* What stays of finding 3: `fd_get` itself (no lock, no reference) is used by most system calls that then call into the file (`read`, `write`, `ioctl`, `lseek` ...);
  a sibling thread closing the descriptor during such a call can free the `File` under it. That is the kernel's rule from before the growing table (one
  reference per descriptor, calls borrow it) and changing it means every call site; not done here.
* Not done: the kernel selftest that forces a yield inside `fd_get`'s unlocked window to prove finding 1 (it needs a test hook inside `fd_get`; left out to keep the
  hot path as it is until somebody wants it).

### Things that were checked and are right

* `fd_reserve`'s race rule (`p->fd_cap == cap` under the lock, the loser frees its block and looks again), the order of the three stores (interrupts are off, one CPU).
* `process_fork_ex`: the child's table is reserved outside the lock and the length compared again under it; when the reservation or the address space fails the
  child's references are dropped (`file_put` loops) and `process_free` releases the table.
* `fd_install` against `RLIMIT_NOFILE`: a soft limit lowered below open descriptors keeps the old ones and fails new ones with `EMFILE`; `dup2` beyond it `EBADF`,
  `F_DUPFD` from beyond it `EINVAL` (golden output `fds_many`).
* `SCM_RIGHTS` (kernel/linux/net.c): a descriptor that cannot be installed is `file_put`, the rest of the array too: no leak when the table cannot grow.
* `close_range` clamps to the table; `select` ignores bits beyond the table as Linux does; `poll` refuses more entries than the soft limit.
* `exec`: the close-on-exec loop reads the length every round; the table is kept (finding 13).
* Pids: `g_next_pid` only grows (an `int`: 2^31 processes), so a pidfd's number never names another process.
* Anonymous pipes: `git diff 9c97f68 -- kernel/fs/pipe.c` changes nothing a pipe pair can see (`is_reader`/`is_writer` are the open mode, `POLLHUP` is as before for a
  pipe: `!p->fifo`). FIFOs: the open rules, the undo of an interrupted open, `g_fifos` when the last opener goes (also by a killed process: `pipe_close` runs from
  `process_finalize`), unlink while open (the pipe is found by the node, which the open files hold).

## What was added

* **Wait queues** (finding 4): one queue for all signalfds, woken by `deposit` in kernel/sys/signal.c (`signalfd_notify`: a test of a counter when no signalfd
  exists), and one for all pidfds, woken by `process_finalize` (`pidfd_notify`). One shared queue each, because a file's queue has to live as long as the file and
  both outlive the process they speak about; a waiter woken for somebody else polls again.
* **`pidfd_getfd`** (438): a duplicate of another process's descriptor, close-on-exec. Permission: root or the same user id (Linux asks ptrace's attach permission).
* **Native `mknod`** (`SYS_mknod` 123, registered from kernel/linux/l3.c with `syscall_register`), `mknod`/`mkfifo` in the project's libc, the **`mkfifo`** command.
* FIFO poll (findings 7 and 8), checked against the real Linux of WSL in `tests/linux/golden/fds_more.txt`.

## Tests

`python tools/qemu-slot.py -- python tests/run_all.py linux_fds` (the four of the first round and):

* `linux_fds_review`: `fds_more` (findings 5, 7, 8, 9, 10 and the wake-ups; by its golden output it cannot pass on the kernel before this branch, which answers
  `ESRCH`, `ENOSYS` and `POLLHUP` in those lines and never wakes the `O_RDWR` poller: read off the old code, not run there) and `fds_stress` (four threads growing a fresh
  table from 64 to 4096 slots at once while the main thread forks, six processes in a row), both compared with a real Linux, both run twice with `/proc/kstats`
  compared in between: files, pipes, vnodes, processes, tasks, threads, kernel stacks equal, free pages within 128, no new kernel warning.
* `linux_fds_native_mkfifo`: the `mkfifo` command, `cat f & echo hi > f; wait` in the native shell, `EEXIST`, no pipe left.

## Not done

* The shell test inside the **Linux root** (a real `sh` and `mkfifo` from the user-provided archive): the native shell runs the same line instead.
* Finding 6 (the owner's decision) and the rest of finding 3 (`fd_get` borrowed by ordinary calls). `signalfd`: a signal that is in the set but not blocked is read by whoever comes first, a read or the handler (Linux: the handler).
* `waitid(P_PIDFD)` on a pidfd with `O_NONBLOCK` should answer `EAGAIN` instead of waiting (waitid(2)); not checked here.
