# Hardening

The PC this OS is for has no serial port, and nobody is watching its screen when something goes wrong, so the failures that matter most are the ones that leave no
trace: a hang, a silent reset, a panic that is never seen. This page is about making the kernel survive programs that are careless or hostile. It says what the kernel
checks now, what was found and fixed, how the tools that find such things work (`sysfuzz`, `/proc/kstats`, `/proc/vnodes`) and what is still open. The last two sections,
**State when paused** and **How to resume**, list what is done, half done and not started.

| Part | State |
|------|-------|
| Warnings that do not stop the machine (`KWARN`), `/proc/kstats`, `/proc/vnodes` | done |
| Allocator checks (double free, damaged free lists, `heapdebug`), recycled kernel stacks, TLB flushes | done |
| Backtraces that cannot fault | done (the fault path inside it has never been triggered) |
| System call checks that the fuzzer and a review found missing | done; what is left is under "Open" |
| `execve` limits: 32768 entries, 128 KiB per string, 512 KiB in all | done, tested |
| `sysfuzz`, the system call fuzzer, and `tools/fuzz.py` | first version, passing; needs many more seeds |
| Resource limits and an OOM killer | not started (`kernel/mm/oom.c` only counts) |
| SMAP, `stac`/`clac`, crash tests for SMEP and SMAP | not started (SMEP was already on) |
| Soft-lockup detector and `panic_on_lockup` | not started (`kernel/sched/watchdog.c` only counts) |
| Panic screens that always appear | partly: since the merge with main a panic during a desktop session **is** drawn (`fbcon_force_on` from the hardware audit; `crash=desktop` and `hwaudit_panic_on_desktop` check it); the rest of item 5 of "How to resume" (second panic, escape state) is not started |

## Warnings and counters

`KWARN(fmt, ...)` and `WARN_ON(cond)` (`kernel/include/kernel.h`, `kernel/warn.c`) are for things that must never happen but that the kernel can survive. A warning is logged as
`[WARNING] file.c:123: message -- during a system call of NAME` with a backtrace, and counted; the first 20 are logged in full, later ones are only counted. Use `panic()`
only when going on is not safe. The hardening tests fail on any warning.

`/proc/kstats` has one `name value` per line: `free_pages total_pages heap_bytes slab_pages tmpfs_pages tasks processes user_threads kstacks files vnodes pipes unix_sockets
inet_sockets ptys warnings oom_kills watchdog_reports`. They are levels, not rates: the tests read them before and after a run and require the counts of objects to come
back. (`oom_kills` and `watchdog_reports` stay 0 until those parts exist.)

`/proc/vnodes` lists every live vnode, newest first, with the first four callers that led to its allocation:
`0xADDR ino=N mode=M refs=R nlink=L size=S <- function+0xOFF function+0xOFF ...`. When the vnode count differs after a test, the lines at the top that were not there before
are the leak.

## What the kernel checks now

**Heap** (`kernel/mm/heap.c`). Requests above 16 MiB fail (they used to wrap the page arithmetic). A free object carries a marker in its second word, so a second `kfree` of it
is noticed (a warning, nothing else happens) instead of corrupting the free list; a free list that was written to is abandoned instead of followed; `kfree` of something that
is not a heap object warns. With `heapdebug` on the kernel command line, freed memory is filled with 0x6B and the fill is checked when the object is handed out again, which
finds writes through stale pointers. The slabs never give pages back to the page allocator.

**Page allocator** (`kernel/mm/pmm.c`). `pmm_free` of a bad, never allocated or already free block warns and returns (it used to panic).

**Kernel stacks and the TLB** (`kernel/mm/vmm.c`). Kernel stacks come from 8192 slots of the stack region, a guard page below each, and are given back when the task is
reaped, so creating threads for months no longer uses up address space and page tables (`kstacks` in `/proc/kstats`). A change to a kernel-half mapping is flushed with `invlpg`
whichever address space is loaded: the kernel half is shared, and a recycled stack slot would otherwise be reachable through a stale entry in another address space.

**Backtraces** (`kernel/arch/x86_64/backtrace.c`, `kprobe.asm`). A frame pointer is followed only into the kernel image, the running task's kernel stack or RAM (the direct map),
and every read goes through `kprobe_read64`: a page fault whose `rip` is that one instruction is turned into a failed read (`trap.c`), so a damaged frame chain ends the walk
instead of causing a second fault in the middle of a panic. `backtrace_capture` records return addresses for the vnode ledger. An NMI is handled before the user-fault hook
(one that arrived in user mode used to be treated as that process's fault).

**System calls.** What the fuzzer and the review that came with it turned up ("What was found" has the story):

| Call | Now |
|------|-----|
| `kill` | A pid above 0x7fffffff is `ESRCH` (it was truncated and hit another process). A group or `-1` is checked per target: the caller signals only what it may (`EPERM` if nothing, `ESRCH` if empty). `init` drops every signal it has no handler for, from anybody. |
| `setpgid`, `getpgid` | Arguments are range checked; a process joins only a group of its own session; session leaders and processes of other sessions are `EPERM`. |
| `ioctl(TIOCSPGRP)` | Only for a terminal that is the caller's controlling terminal (`ENOTTY`), negative is `EINVAL`, a group with no member in the caller's session is `EPERM`. `SYS_tcsetpgrp` itself answers `ENOSYS`. |
| `tgkill`, `sigtimedwait`, `futex`, `nanosleep`, `poll` | `tgkill` checks id ranges and permission. The others saturate their deadlines instead of wrapping (`deadline_from_timespec`), and the futex word must be RAM (`pmm_page_managed`). |
| `sigaction`, `sigreturn` | A handler or restorer at or above `USER_TOP` is refused; a signal frame whose `rip` or `rsp` is at or above it ends the process with `SIGSEGV` (an `iretq` with such a frame would fault inside the kernel). |
| `lseek`, whole-file reads, `readdir` | Offsets are checked for overflow; `vfs_read_file` (the kernel's read-a-whole-file helper, used by the kernel shell) refuses files over 64 MiB; directory reads use stable cookies (`DirEntry.next`), so removing entries while reading no longer skips others. |
| `execve` | See "execve limits". |

**tmpfs** (`kernel/fs/tmpfs.c`, rewritten for this). A file is at most 2 GiB (`TMP_MAX_PAGES` pages; `EFBIG` beyond, and a write that starts below the limit is cut at it), so no
offset arithmetic can wrap and the page pointer table stays at 4 MiB at most. Every allocation failure in create, mkdir, symlink, link, rename and special files unwinds what was
done. The file system as a whole has no size limit yet (see "Not started").

**Sockets, pipes, ptys.** `AF_UNIX` sockets hold file references (`socket_ref`) and are allocated and released in pairs (`sock_alloc`, `sock_release`); `socketpair`, `pipe` and every
error path give back what they took; a socket that was bound but never listened removes its file node on close. The counts feed `/proc/kstats`.

## execve limits

| Limit | Value | Over it |
|-------|-------|---------|
| entries in `argv`, and in `envp` | 32768 each (`EXEC_MAX_ARGS`) | `E2BIG` |
| one string, NUL included | 128 KiB (`EXEC_MAX_STRING`) | `E2BIG` |
| the whole block: the strings, 8 bytes for each pointer, 64 bytes fixed (`exec_footprint()`) | 512 KiB (`EXEC_MAX_TOTAL`; `ARG_MAX` in `limits.h`) | `E2BIG` |

Code: `kernel/include/process.h`, `kernel/sys/syscalls.c` (`sys_execve`), `kernel/sys/exec.c`. `sys_execve` copies each vector into an `ExecVec` (every string at its real length, the
pointer array doubling as it grows) and checks the totals with overflow-safe arithmetic while copying, so an absurd request fails before anything large is allocated. `build_stack`
measures the vectors and writes the new stack in one streaming pass, with no temporary arrays, and populates as much of the initial stack as the arguments need (at least the usual
initial size). `#!` scripts go through the same limits, and the depth of script and interpreter chains is a parameter of `exec_file` instead of a global. A thread that calls
`execve` while another thread is already ending the process frees what it holds and leaves (`EXEC_THREAD_LEAVES`).

In user space `xargs` fills a command line up to 128 KiB or 16384 items per batch (so `seq 1 100000 | xargs echo` works) and no longer duplicates the whole remaining input for every
item; `gapsh` expands `*` over 3000 files, and `ls *` and `rm *` work.

Tests: `user/test/exectest.c` (5000 and 20000 short arguments, a 100 KiB argument, a string of exactly 131071 characters plus NUL and one of one more, 32768 and 32769 entries, totals
of 400, 500 and 600 KiB, 5000 environment entries, a big environment, arguments and environment that are only over the limit together, pointers into the kernel half and into unmapped
memory (`EFAULT`), a `#!` script with 2000 arguments) and the shell checks in `hardening_exec` (`tests/case_hardening.py`).

## The system call fuzzer

`user/test/sysfuzz.c` is installed in `/usr/lib/kernel-tests` (`tools/build.py` installs everything from `user/test/` there, mode 0755; it is in nobody's `PATH`). Three ways to run it:

* `python tools/fuzz.py 1 2 3` boots QEMU once per seed with `text kerneltest=sysfuzz ktargs=seed:N,time:25,workers:2` and prints one line per run (PASS, FAIL, PANIC or TIMEOUT, the guest's
  exit status, the final counts); a failing run also prints the end of the log and the last `worker N seed S starts` lines. Options: `--secs`, `--workers`, `--root`, `--extra heapdebug`
  (fill freed heap memory with a pattern and check it), `--only N` (replay worker N alone). The log of the last run is `build/fuzz-serial.log`, QEMU's own (with `cpu_reset` events) `build/fuzz-qemu.log`.
* `$env:TOUHOU_HARDENING_FUZZ = "1"; python tests/run_all.py hardening_sysfuzz` is the same as a test (seeds 1, 2 with `heapdebug`, and 3, 40 s each). It is not in the default suite.
* By hand: `kerneltest=sysfuzz ktargs=seed:5,time:60` on the kernel command line, or `/usr/lib/kernel-tests/sysfuzz --seed 5 --time 60` as root in a running guest.

With `kerneltest=NAME`, `init` (before the clock, the login and the desktop) runs `/usr/lib/kernel-tests/NAME --config ARGS` as root, collects its orphans, waits a second for the last lines of the
report to leave the serial port and powers off with the program's exit status (`gap_reboot(0x100 | status)`; `Qemu.wait_exit()` returns it). So any new program in `user/test/` can be run the same way.
Configuration keys (`ktargs=k:v,k:v` or `--config`): `seed`, `time` (seconds, default 30), `workers` (1 to 4, default 2), `calls` (per worker, default 1500), `root` (the workers keep uid 0), `trace`
(print every call before it is made), `only` (run just the worker with this number), `stuck` (milliseconds without progress before a worker is killed, default 1500), `verbose`.

**Never run it on the real PC.** It is for QEMU: as root it can mount, unmount and set the clock, and even as an ordinary user it kills processes and fills `/tmp`.

### What it does

A *supervisor* (root) starts *workers* one after another, up to `workers` at a time. A worker puts its standard streams on `/dev/null` (unless `trace`), becomes the throw-away user 5000 (unless
`root`), builds a zoo of memory regions (read-write, read-only, `PROT_NONE`, one that ends exactly where a buffer ends, a file mapping, a shared mapping) and descriptors (scratch files, pipes,
sockets of both families, a pty pair, `/proc` and `/dev` files), and then makes `calls` system calls. One call in ten starts a *scenario*, a run of related calls that random ones rarely form:

* **file**: seek, write, read, truncate, map, `dup2`, `fcntl`, unlink on one file, with offsets at every power of two;
* **sock**: two sockets of either family bind, listen, connect, accept and exchange data in odd sizes, with options and `shutdown` in any order;
* **pty**: a pair with control characters in the data, termios and window size changes, `TIOCSPGRP`, closing either end first;
* **proc**: children that exit, crash, sleep or are killed while the worker waits for them in every flavour, signals to them and their groups, `setpgid`;
* **mem**: overlapping and fixed maps, `mprotect`, `munmap` of pieces, `brk`, a fork with all of it;
* **exec**: `execve` in a child with vectors at and beyond the limits.

The other calls come from a table of about 100 system calls with typed argument generators (null, unaligned, kernel-half and one-byte-past-the-end pointers, lengths of zero, one, the room, the
room plus one and huge or negative ones, descriptors that are closed or out of range, odd flags, overlapping buffers, pids of itself, its children and nonsense, structures that are valid in shape
with a mutated field); 7% of them use any number below 140, defined or not, with random arguments, and 3% numbers at the edges (`-1`, `2^31`, `2^63`) or random 64-bit ones. `sigreturn` is called with
signal frames whose registers, segments, `rip`, `rsp` and flags were mutated. Threads (with a sane body) and forked children race with the main thread; a helper thread sends the main thread `SIGUSR2`
when it has been inside one call for more than 120 ms, so blocking calls end with `EINTR` instead of stalling the run. Output buffers have canaries on both sides.

Everything is derived from the seed: worker number N of seed S always makes the same calls (the kernel's timing still varies, so a replay is not guaranteed to fail again). The supervisor prints
`sysfuzz: worker N seed S starts` before each worker; after a panic the last few such lines (as many as there were workers at a time) name the candidates, and `--only N` replays one of them alone with
every call printed before it is made (`trace`). Each worker keeps a ring of its last 16 calls in shared memory (a mapped tmpfs file), which the supervisor prints for a worker that is stuck or violated a rule.

### What counts as a failure

* a kernel panic (`INCIDENT: KERNEL PANIC` in the log), or a hang: a worker that is still there 5 s after `SIGKILL` (`UNKILLABLE`), or no verdict before the harness's timeout;
* a `[WARNING]` in the kernel log (`/dev/kmsg`), or `warnings` in `/proc/kstats` going up;
* objects that do not come back: open files, pipes, `AF_UNIX` sockets, ptys, processes, user threads, kernel stacks, tasks and vnodes must be equal before and after (`/proc/vnodes` then shows which
  ones are new). Tolerances, chosen by looking at what a clean run leaves: `AF_INET` sockets may be 64 more (TCP connections in their closing states outlive the run), the heap 256 KiB more, and
  at most 128 pages of RAM may be missing that neither the slabs nor tmpfs files account for (clean runs show about two);
* the kernel writing outside the buffer it was given (canaries), an error result that is not in -4095..-1, an `mmap` result that is not page aligned or not below `USER_TOP`, `getpid` changing.

The workers themselves may die of anything (almost all do; a 30 s run is 45 to 65 short-lived workers and 14000 to 19000 calls, with zero to four stuck in a sleeping call that the supervisor kills).
After the run the supervisor kills what is left, removes its scratch directory (`/tmp/sysfuzz.PID`) and whatever the fuzz user left in `/tmp`, waits for the zombies and compares `/proc/kstats`.
It prints `SYSFUZZ PASS` or `SYSFUZZ FAIL` and exits 0 or 1 (2 if it could not set itself up).

### What it does not do

* It cannot make kernel allocations fail, so the `ENOMEM` paths are only exercised by accident (see "How to resume": failure injection).
* There is one CPU, so races are between preempted tasks and threads, never between CPUs.
* Nothing that needs hardware: block devices, sound and USB device files are not in its path list (only a root run opens `/dev/fb0` and the input device), and nothing arrives from the network except
  what its own sockets send over loopback.
* A root campaign has never been completed (a root run was used while debugging). Paths that root may change are generated inside the scratch directory, and `mount`, `umount` and `settime` are only tried as root.
* The random calls never name pid 1, the supervisor or "everybody" (`kill(-1)`): a successful one would end the run. The kernel's refusal of those is therefore not tested by the fuzzer any more; the
  hole that let an ordinary user take init down was found by an earlier version of it, before that filter existed. A small test of its own is on the list.

## What was found

Found by running the fuzzer (each one a crash or a miscount that the first runs produced):

| What was wrong | Fixed by |
|----------------|----------|
| **Kernel panic: `init (pid 1) exited with status 0x3`.** An ordinary user could point the foreground group of a pty it owned at group 1 (`TIOCSPGRP` took any number), so that a control character such as Ctrl-\ written through the master sent `SIGQUIT` to init (status 0x3 is `SIGQUIT`); `kill(-pgid)` and `kill(-1)` had no permission check per target; `setpgid` joined any group. | `init` drops signals it has no handler for; `signal_permitted` per target in `kill`, `signal_group_from` for groups and broadcasts; `setpgid` stays inside the session (`pgrp_in_session`); `TIOCSPGRP` only by the terminal's owner. |
| **Kernel panics (#GP, #PF) in `tmp_write`.** An offset above 4 GiB lost its upper bits in the page index, and sums of offset and length wrapped. | `TMP_MAX_SIZE` and `EFBIG`, `grow_table`, every allocation checked. |
| The supervisor itself once died (exit status 99) in the middle of a run. A worker had signalled its own process group through a pty's foreground group. | Taken to be the same hole as the first row (`TIOCSPGRP` ownership); never seen again, but not proven. |
| Vnodes and files that did not come back; `remove_tree` in the fuzzer skipped entries. | The directory read used an index, so removing entries while reading shifted the rest: stable cookies in tmpfs and the VFS. The rest of the differences were the harness itself (a pty directory created once, files made through relative paths in `/tmp`). |

Found by reading code while the fuzzer was being built, and fixed (none of these was seen as a crash):

| What was wrong | Fixed by |
|----------------|----------|
| `kill` truncated the pid to 32 bits, so a huge number hit a real process. | `ESRCH` above 0x7fffffff. |
| `sigaction` accepted any handler and restorer; `sigreturn` accepted any `rip` and `rsp`. A value in the kernel half makes the `iretq` fault inside the kernel. | Refused / `SIGSEGV`. |
| `futex` on an address that is not RAM (a device mapping) was accepted. | The word must be RAM. |
| `nanosleep`, `poll`, `sigtimedwait` and futex timeouts wrapped around for huge values. | Saturating deadlines. |
| An NMI arriving in user mode was treated as that process's fault. | Handled first. |
| `kmalloc` of more than 16 MiB wrapped the page arithmetic; a double `kfree` corrupted the free list; `pmm_free` of a wrong block panicked. | See "What the kernel checks now". |
| Kernel stacks were never reused (address space and page-table pages for every thread ever made), and a recycled mapping could still be in another address space's TLB. | Stack slots; `flush_one`. |
| `lseek` overflowed; whole-file reads had no cap; `tgkill`, `getpgid` and `setpgid` took any ids. | Checked. |
| `AF_UNIX` sockets used the descriptor's `File` without holding a reference (a close in another thread could free it under a running call); `socketpair` leaked on several paths; a bound socket that never listened kept its file node; `vfs_pipe` leaked when its second allocation failed. | `socket_ref`, `sock_alloc`/`sock_release`, cleanup on every path. |
| `execve` had small fixed limits and built the stack from temporary arrays. | "execve limits". |

### Open

Known and not fixed. The first four are the ones to do first.

1. **A probable hang under `heapdebug`.** Seed 105 with `--extra heapdebug` (2 workers, 30 s): in the last campaign it printed the first line of its report and nothing after that, not even the
   ten-second progress line, no panic text and no verdict. The campaign took 445 s for six seeds where a normal run takes about 60 s, so that run most likely sat out the harness's 150 s timeout with
   the guest alive but silent: a hang of the kernel (or of its serial output) in the first seconds of the run. It was not examined (the pause came) and not repeated; the two earlier failures of
   this seed were vnode miscounts caused by the harness, since fixed. The seed's call sequence changed with that fix, so it may be a different path now. Run
   `python tools/fuzz.py --secs 30 --extra heapdebug 105` first thing: on a failure the tool prints the last `worker N seed S starts` lines, `--only N` replays one of those workers alone with a trace, and
   `build/fuzz-qemu.log` says whether the guest reset itself (the harness runs QEMU with `-d guest_errors,cpu_reset`). If it does hang, that is the case for the lockup detector (below) to catch.
2. **Borrowed file pointers.** `fd_get` returns a pointer without a reference, and `lseek`, `fstat`, `readdir`, `dup`, `dup2` (which is also racy), `fcntl`, `ftruncate`, `ioctl`, `poll` and `mmap`
   use it across calls that can sleep, so a thread that closes the descriptor at the wrong moment leaves them with a freed `File`. `fd_get_ref` and `file_put` exist (the sockets use them).
3. **Range arithmetic in the address space.** `uvm_unmap`, `uvm_protect` and `uvm_populate` add address and length without a wrap check; `do_mmap` populates file mappings eagerly and without a bound
   (a 4 GiB map of a file reads all of it), does not check the file offset for overflow and does not hold a reference to the descriptor. The `exec` loader does not look at a huge `p_memsz` before it starts mapping.
4. **Other leaks and races on rare paths.** `process_fork`, when it fails late, unlinks the child as if it were at the head of the process list; opening the slave of a pty races with the release of the
   master; `sys_ptyopen` leaks a vnode when an allocation fails; `sys_pipe` and other `ENOMEM` paths have never run (failure injection would show them).
5. `vnode_put` still panics on a negative reference count (it could warn). The other `panic()` calls are boot-time failures, the deliberate crash tests, the exit of init, a zombie being scheduled and
   `virt_to_phys` of a pointer that is neither in the direct map nor in the kernel image (a driver bug).
6. The privilege checks of `SYS_netctl` (interface, route and ARP changes) were not reviewed.
7. TCP connections in `TIME_WAIT` and similar states outlive a run (the reason for the tolerance of 64 `AF_INET` sockets).
8. The `kprobe` fault path (`__kprobe_fault`) has never been taken: no test makes a backtrace meet an unmapped frame pointer.

## Tests

* `python tests/run_all.py hardening_exec` (about 30 s): `exectest` plus the shell checks for `xargs`, `ls *` and `rm *` with thousands of files and for `E2BIG`.
* `hardening_sysfuzz`: opt-in, see above. It has never been run as a test; equivalent runs were made with the scripts that `tools/fuzz.py` was cleaned up from.
* The suites that cover what was changed: `jobctl`, `jobctl_tools` (signals, groups, terminals), `threads_*`, `net_loopback`, `storage_posix_semantics` (tmpfs), `crash_*` in `tests/case_core.py` (the
  panic screen and the backtrace), `kshell_*`, `selftest`.

## State when paused

Paused on 2026-10-23 in the evening, at the request of the user. The branch `worktree-agent-a4adbf8e4aa5804b3` is committed and builds (`python tools/build.py`), and `main` was last merged at `c7609d6`
(54 commits behind at the pause; `git merge-tree --write-tree HEAD main` said the merge was clean). Nothing is switched on that was not tested: SMEP was already enabled by the original `cpu_init`,
and `oom.c` and `watchdog.c` only hold the counters that `/proc/kstats` shows.

* **Done**: everything described above under "What the kernel checks now", "execve limits" and "The system call fuzzer"; the `/proc/kstats` and `/proc/vnodes` tools; `docs/JOBCONTROL.md` follows the
  new signal and terminal rules; `hardening_exec`.
* **Half done**:
  * The fuzzer works and passes, but it has had few seeds. Passed: 1 to 3, 11 to 14 and 21 to 24 (20 to 25 s, plain) on the kernel revisions of their day, and 102 to 104, 106, 111 to 113 and 121 to 125
    (30 s, with `heapdebug`) on the final kernel. Seed 101 once lost the end of its report (serial output cut off at power-off; `init` waits a second now) and was not repeated. Seed 105 hung (Open, 1).
    Everything was with two workers as an ordinary user. No root campaign, no run with three or four workers, nothing longer than 40 s.
  * `hardening_sysfuzz` is written and syntax checked, and has not been run. `tools/fuzz.py` is the cleaned-up version of the scripts that made the runs above; it was written at the pause and only
    its syntax and `--help` have been checked, so the first real run may need a fix.
  * The review items under "Open" are written down, not fixed.
* **Not started**:
  * Resource exhaustion: limits per user (processes, threads, memory), a reserve of pages for the kernel, the OOM killer (`oom_kills` in `/proc/kstats`), a size limit for tmpfs as a whole, and the guest
    programs and tests for them (`memhog`, a fork bomb, filling `/tmp`, running out of descriptors). `storage_small_ram` must keep passing.
  * SMAP: `CR4.SMAP`, `stac`/`clac` around the user-copy routines, `clac` at the kernel entries, and crash tests `smep` and `smap`.
  * The soft-lockup detector with `panic_on_lockup` (off by default).
  * Panic screens: see below; no code has been written.
  * A test that an ordinary user cannot take init down (`kill(1)`, `kill(-1)`, `setpgid` into init's group, `TIOCSPGRP` of a pty to group 1); the random calls no longer try these.
  * Failure injection for allocations (a `failalloc=N` command line word), so the fuzzer can reach the `ENOMEM` paths.
* **Tests run**, all on this machine (`PASS` unless said):
  * after the `execve` work: `hardening_exec`, `shell`, `utest`, `jobctl`, `jobctl_tools`, `apps_cli`, `manual`, `threads_utest`, `pager`;
  * after the groundwork commit: `selftest`, `utest`, `kshell_serial`, `kshell_keyboard`, `desktop_boots`, `jobctl`, `jobctl_tools`, `net_loopback`, `threads_utest`, `threads_selftest`;
  * after the signal, terminal, tmpfs and socket changes (commit `ca96648`): twenty tests: `jobctl`, `jobctl_tools`, `shell`, `utest`, `threads_utest`, `threads_selftest`, `net_loopback`, `desktop_terminal`,
    `login_text`, `storage_posix_semantics`, `kshell_serial`, `kshell_keyboard`, `selftest`, `manual`, `hardening_exec`, `apps_cli`, `pnote_edit`, `pager`, `lock_screen`, `desktop_boots`;
  * after that only fuzz campaigns ran. **The suite was not run on the final head** (`c889090`, which rewrote the backtrace code and added the vnode ledger), and in particular the `crash_*` tests of
    `tests/case_core.py` were not run on it. By reading, the output format of `dump_backtrace` is unchanged ("backtrace:" and numbered lines), but that is not a test.
* **Merged on `cloud/staging` (2026-10-24, `f8905d4`)**, Linux, QEMU 8.2 TCG, `TOUHOU_TIMEOUT_SCALE=3`: `crash,hardening,selftest,utest,shell,jobctl,kshell,threads`
  pass (all nine `crash_*` on the final backtrace code), and `hardening_sysfuzz` (`TOUHOU_HARDENING_FUZZ=1`, three seeds) passes. One semantic conflict with main: main's
  `hwaudit_panic_on_desktop` brought the machine down by typing `kill -9 1` as root, which init now shrugs off. Both intents are kept: init stays immune, and a new
  `crash=desktop` (the kind item 5 below planned; `kernel/main.c`) panics 20 s after the window server took the screen; the test boots with it and checks the same
  pixels. The init protection test (item 4) is still not written.

## How to resume

1. `git merge main`, `python tools/build.py`. Tests that wait for a shell prompt need `text` on their command line. Two QEMU runs in one checkout share `build/esp` (the harness writes
   `cmdline.txt` there), so run only one at a time, or copy the sources to another folder for a second one. The guest's clock runs slowly when the host is busy: wait for results, not for time.
2. Check the head first: `python tests/run_all.py crash,kshell,selftest,hardening_exec,jobctl,shell,utest,threads` (the `crash_*` tests exercise the rewritten backtrace), then the whole suite (about an hour).
3. Fuzz more: `python tools/fuzz.py --extra heapdebug --secs 30 105` (Open, 1), then `python tools/fuzz.py 201 202 203 204 205`, `--workers 4`, `--secs 120`, and `--root` (check the log for `UNKILLABLE`,
   `LEAK`, `INCIDENT`; after a panic replay with `--only N`). A failing run's logs are `build/fuzz-serial.log` and `build/fuzz-qemu.log`. Add every bug found to "What was found".
4. Write the init protection test (a guest program in `user/test/`, run as `kerneltest=initguard` or from a test shell as an ordinary user): `kill(1, SIGKILL)`, `kill(-1, SIGTERM)` from a throw-away session,
   `setpgid(0, 1)`, `ioctl(pty, TIOCSPGRP, &1)` followed by a Ctrl-\ through the master; afterwards `/proc/1/status` must still exist and no kernel warning must have been logged.
5. The remaining features, in this order of value for a PC without a serial port (the original order was 2, 3, 4, 5):
   * **Panic screens** (small, the biggest real gap). Today `panic()` and `panic_frame()` call `cli()`, stop silently on a second panic, and print through `printk`: the in-memory log, the serial port and
     `fbcon_write`. There are no locks in that path (one CPU, interrupts off), `printk` formats into a stack buffer, `ksym_lookup` reads a static table and `incident_quip` and `task_print_current` read static data and
     `current` (a corrupted `current` would fault, and a second fault halts silently); but `fbcon_write` draws nothing while the
     console is disabled, and `kernel/drivers/fbdev.c` disables it whenever the window server holds `/dev/fb0` (`FBIO_GRAPHICS`). So **a panic during a desktop session shows nothing on the screen**
     (the last desktop frame stays), and a PC without a serial port gives no trace at all. Plan: a `fbcon_panic_reset()` called first thing in both panic functions: enable the console, reset the
     escape and UTF-8 state machine (a panic can interrupt `fbcon_write` in the middle of a sequence, which would swallow the red banner), make drawing go to the front buffer (`g_fb.back = g_fb.front`), redraw the
     cell buffer so the last log lines are above the message. Make the second panic print a short line ("PANIC WHILE PANICKING" plus the two `rip`s) straight to the serial port and the framebuffer instead
     of halting silently; use one depth counter for `panic()` and `panic_frame()` (they have one each). Extend `crash_test`
     in `kernel/main.c` (`crash=KIND` on the command line, tests in `tests/case_core.py`) with kinds that need this: `desktop` (panic after the window server took the screen: check the screen by sampling
     pixels of the banner colour with the harness's `screendump`, see `tests/debug_pixels.py`), `escape` (panic in the middle of an escape sequence), `nested` (a fault inside the panic text), `badframe`
     (a backtrace from a garbage frame pointer: exercises `__kprobe_fault`), `stack` (kernel stack overflow into the guard page: arrives as a double fault on its own stack), `heapheld` (panic with the heap in a half-updated state).
   * **SMAP** (`kernel/arch/x86_64/cpu.c` sets `CR4.SMEP` today; `CPU_HAS_SMAP` exists in `cpuinfo.h`). Kernel code reaches user memory through two assembly routines only, `__copy_user` and
     `__clear_user` in `kernel/arch/x86_64/uaccess.asm` (the C helpers in `kernel/sys/uaccess.c`, including `strnlen_user` and `strncpy_from_user`, all call them); a fault inside
     `[__uaccess_start, __uaccess_end)` that demand paging cannot satisfy is redirected to `__uaccess_fault`. Put `stac` at the start of both routines and `clac` before each `ret` and at `__uaccess_fault`;
     put `clac` at `isr_common` and the `syscall` entry as well (`MSR_SFMASK` is 0x47700, which already clears `AC` on `syscall`; do not rely on the CPU doing it for interrupts); then set `CR4.SMAP`.
     Every place that dereferences a user pointer directly (drivers' `ioctl` handlers, `/proc` writers, the `exec` and signal code) will fault once it is on: run the whole suite and the fuzzer with it on to
     find them (QEMU's `-cpu max` and the Skylake model both have SMAP). `process_pagefault` (`kernel/sys/process.c`) today demand-pages a user address even when the fault came from kernel code outside the
     uaccess range; refuse that case, so a stray kernel access to user memory is a panic with a name instead of a quiet success. Crash tests: `crash=smep` (the kernel jumps to a user page) and `crash=smap`
     (the kernel reads a user address outside the uaccess code); both must panic with a message that names the feature.
   * **Resource exhaustion and the OOM killer.** Today an empty page allocator makes `uvm_fault_locked` (`kernel/sys/uvm.c`) log `[Yuyuko] out of memory while mapping page ...` and leave the fault
     unresolved, so the faulting process gets `SIGSEGV` (a fault inside a user copy gives `EFAULT`); nothing else about running out of memory has been examined. The existing limits are system-wide
     (256 processes, `MAX_PROCS`; 1024 threads and 256 per process, `shared/syscall.h`; 64 descriptors per process, `MAX_FD`): there is none per user, and nothing bounds the memory of a process, the
     total size of tmpfs or the buffers of pipes and sockets, so memory is what a hostile or broken program can use up. Plan: a reserve of pages that only the kernel's own allocations (stacks,
     page tables, heap) may use, while allocations made on behalf of a process (page faults, `mmap` population, tmpfs, pipe and socket buffers) fail at the watermark; at the watermark an OOM killer picks the
     process with the most pages (never pid 1, kernel threads or the caller's session leader if there is another choice), logs `[OOM] killed NAME (pid N, M MiB)`, sends `SIGKILL`, counts it in `oom_kills`,
     and lets the allocation wait for it with a timeout instead of killing again; nothing to kill means `ENOMEM` (a fault gets `SIGSEGV`). A size limit for tmpfs as a whole (half of RAM) so that filling `/tmp`
     gives `ENOSPC` rather than killing an innocent process; per-user limits on processes and threads. Guest programs in `user/test/`: `memhog`, `forkbomb`, `tmpfill`, `fdhog`, run as an ordinary user with
     `mem=64M`; the machine must answer afterwards, kstats must return to their old values, and `storage_small_ram` must still pass.
   * **Soft-lockup detector.** Tick based: the timer interrupt counts consecutive ticks in which the same task is running in kernel mode (or interrupts were found disabled for too long, measured with the TSC
     in `irq_save`/`irq_restore` as a debug option); after N seconds it logs one `[WATCHDOG]` line with a backtrace, counts it in `watchdog_reports`, and panics only if `panic_on_lockup` is on the command line
     (default off). A user process spinning in user mode must not be reported. A hard lockup (interrupts off for good) needs an NMI source; QEMU's TCG does not emulate performance counter NMIs, so it cannot be tested
     here and is better left out. Test: a `crash=lockup` kind that spins in the kernel with interrupts on, once with and once without `panic_on_lockup`.
6. Failure injection: a `failalloc=N` command line word that makes every Nth `kmalloc` and page allocation fail, and a `sysfuzz` option that uses it; this is the way to reach the `ENOMEM` paths (Open, 4).
7. Where things are: `kernel/warn.c`, `kernel/mm/{heap,pmm,vmm,oom}.c`, `kernel/sched/watchdog.c`, `kernel/arch/x86_64/{backtrace.c,kprobe.asm,trap.c,cpu.c}`, `kernel/panic.c`, `kernel/printk.c`,
   `kernel/gfx/fbcon.c`, `kernel/drivers/fbdev.c`, `kernel/sys/{syscalls,exec,signal,uaccess,futex,thread,process}.c`, `kernel/fs/{tmpfs,vfs,pipe,socket}.c`, `kernel/drivers/{tty,pty}.c`, `user/test/*`,
   `user/sbin/init.c` (the `kerneltest=` block), `tools/fuzz.py`, `tests/case_hardening.py`.
