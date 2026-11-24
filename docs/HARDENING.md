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
| Resource limits (`getrlimit`, `setrlimit`, `prlimit`, `ulimit`) | done, tested (second round, 2026-11-17: "Resource limits") |
| A reserve of pages, the OOM killer, tmpfs limited as a whole | done, tested in QEMU (second round: "The OOM killer") |
| SMAP, `stac`/`clac`, crash tests for SMEP and SMAP | done, tested in QEMU, not yet tried on the Dell (second round: "SMAP and SMEP") |
| Soft-lockup detector and `panic_on_lockup` | done, tested in QEMU; a hard lockup is the TCO watchdog's (second round: "The lockup detector") |
| Panic screens that always appear | main has the part that mattered (the panic path switches the console back on, `fbcon_force_on`); the nested-panic and escape-sequence cases are not started ("How to resume", item 5) |

## Warnings and counters

`KWARN(fmt, ...)` and `WARN_ON(cond)` (`kernel/include/kernel.h`, `kernel/warn.c`) are for things that must never happen but that the kernel can survive. A warning is logged as
`[WARNING] file.c:123: message -- during a system call of NAME` with a backtrace, and counted; the first 20 are logged in full, later ones are only counted. Use `panic()`
only when going on is not safe. The hardening tests fail on any warning.

`/proc/kstats` has one `name value` per line: `free_pages total_pages heap_bytes slab_pages tmpfs_pages tasks processes user_threads kstacks files vnodes pipes unix_sockets
inet_sockets ptys warnings oom_kills watchdog_reports`. They are levels, not rates: the tests read them before and after a run and require the counts of objects to come
back. `oom_kills` counts the processes the OOM killer ended and `watchdog_reports` the soft lockups reported (both from the second round, below); `/proc/oom` says more about the killer.

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
done. All the tmpfs instances together (the root, `/tmp`, `/linux`) may hold half of the RAM and no more: `tmp_write` and `tmp_mmap_phys` stop there (a write that cannot get
a page is cut short, or fails with `ENOSPC`), and `df` reports the same figure. The pages of a file come from the allocator's user class (see "The OOM killer"), so a runaway
`dd` into `/tmp` ends with `ENOSPC` while the machine still has the other half, and a shortage of free pages caused by something else is the killer's business.

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

## SMAP and SMEP

*Second round, 2026-11-17.* With **SMEP** (supervisor-mode execution prevention, on since the first version) the kernel cannot run code from a user page, and with **SMAP**
(supervisor-mode access prevention, `CR4.SMAP`, switched on by `cpu_init` when CPUID leaf 7 (EBX bit 20) offers it) it cannot read or write a user page either, except
while `EFLAGS.AC` is set. Together they close the cheapest ways to turn a kernel bug into a program's win: a stray pointer that happens to hold a user address is a panic with
a name instead of a read of whatever a hostile program put there, and a jump through a pointer a program could plant cannot land in the program's own code. Written from the Intel SDM
(vol. 3A 2.5 and 4.6.1; STAC and CLAC in vol. 2).

**Who touches user memory.** Two assembly routines and nothing else (`kernel/arch/x86_64/uaccess.asm`): `__copy_user` (`rep movsb`) and `__clear_user` (`rep stosb`), reached through
`copy_from_user`, `copy_to_user`, `clear_user`, `strnlen_user` and `strncpy_from_user` (`kernel/sys/uaccess.c`). Each opens the window with `stac` just before its string
instruction and closes it with `clac` right after; the fixup `__uaccess_fault` (where a fault that demand paging cannot satisfy ends) closes it too. Nothing runs inside the window
but that one instruction. The audit that found this (every one of the roughly 470 uses of the system call argument macros `A0`..`A5` and every cast of an integer to a pointer in the system call
files, the Linux personality, the sockets, the drivers' `ioctl` handlers, the signal frames of both personalities, `exec`, the futex code) found **no direct dereference of a
user pointer**: every handler copies through the helpers, the signal frames use `copy_to_user`/`copy_from_user`, the new stack of an `exec` and the futex words go through the direct
map (`uvm_write`, `vmm_translate` + `phys_to_virt`), and the ioctl handlers receive a kernel copy of the argument. The audit was made again on the tree after the Linux `/proc` and exec-by-markers
work was merged (main f4629f6): `kernel/linux/procfs.c` reads a process's memory (`/proc/<pid>/cmdline`, `environ`) through that process's page tables and the direct map (`read_user`), and
`exec.c`'s `stack_word` reads the new stack the same way, so nothing there touches a user address either; and once more after main 9d69965 (the `.bss` of a program is demand-zero: `elf_map` only
fills the pages that hold file data): that change decides which pages `uvm_populate` maps at `exec`, and the rest are filled by `uvm_fault` at the first touch, by the program or by a copy routine, so a
demand fault inside the copy window is now common. That is the path the window was built for (the fault is taken with `AC` set in the frame, the handler runs with it clear, `iretq` gives it back and the
`rep movsb` goes on), and main's `rig_big_bss` (the kernel reads untouched `.bss` pages, one a gigabyte away) runs it. And a third time after main 06f5e56 (the L3 calls for Python and gcc: `kernel/linux/l3.c`,
`fsops.c`, `ioctl.c`; the ext4 driver; a word-wise `memcpy`): every new call copies through `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `at_path` or `lnx_iov`, the ext4 driver never sees a user
pointer, and `memcpy` and `memset` are only for kernel memory (the copy routines of `uaccess.asm` are separate). So no file needed a fix for SMAP; the proof that nothing was missed is the suite run with
SMAP on (below). (The one change that merge needed was not a user access but an allocation: see "The reserve" below, `lnx_mmap`.)

**Interrupts and exceptions.** The CPU does not clear `AC` when it enters a handler, and any ring may set `AC` in its own flags, so `isr_common` starts with `clac` (every
vector goes through it, NMI and double fault included): without it a program that sets `AC` and waits for a tick would run the kernel's handlers with SMAP effectively off, and a
tick inside the copy window would run its handler with the window open. The interrupted code gets its own `AC` back at `iretq`. `SYSCALL` clears `AC` through `IA32_FMASK`
(0x47700 has bit 18). `trap_handler` warns if it ever finds `AC` set (`tests/case_hardening2.py` sets it in user mode and waits for ticks). A protection fault (the page is present) taken
in kernel mode on a user address with `AC` clear is reported as **`SMAP VIOLATION: the kernel read the user address 0x... outside the copy window`**, a fetch from a user page as **`SMEP VIOLATION`**,
both before the fault is offered to demand paging. A page that is *not* present is left to the old path (a null or garbage pointer stays a `BOUNDARY VIOLATION: not-present page ...`, which
the `crash_pf` and `crash_null` tests expect), and a valid user page that was never touched is demand-paged and then faults again as a present page, which is when the message above appears.

**`STAC` and `CLAC` are `#UD` on a CPU without SMAP**, so every use is guarded by one byte, `g_smap_enabled` (set by `cpu_init` when the CPU has SMAP and `smap=off` is not on
the command line): a branch on a flag that never changes, in the copy routines and at the top of `isr_common`.

**Command line:** `smap=off` (or the Linux word `nosmap`) leaves CR4.SMAP clear and runs no `stac` or `clac` at all: the way to survive a bad day on real hardware (a missed access
shows up as a panic that names `SMAP VIOLATION` and the address; the `dmesg` line that says it is on is `[Boundary] SMAP on: ...`, and with the word `SMAP off: smap=off is on the command
line`). **Crash tests** (`crash=KIND`, `kernel/crash_hardening.c`): `smap` (a plain load from a user page), `smep` (a call into an executable user page), `smap-window` (every copy routine in
every way it can end, successful, faulting, clearing and string, and then a plain load: it must still be refused, which proves each closes the window). `acflag` (`user/test`) is the user program
that sets `AC` and makes system calls, including failing ones, while the timer ticks.

**What it does not cover.** A user access that goes through the direct map (the physical page of a user mapping is reachable by its kernel address) is not an access to a user page
as far as the CPU is concerned: SMAP cannot see it, so the helpers that do it (`uvm_write`, the futex words) check the mapping themselves. SMAP guards one CPU's view; nothing here is
about the speculative reads that SMAP does not stop either.

## Resource limits

*Second round.* `getrlimit`, `setrlimit` and `prlimit` (`SYS_getrlimit` 120, `SYS_setrlimit` 121, `SYS_prlimit` 122; `kernel/sys/rlimit.c`, `shared/abi.h`; libc `<sys/resource.h>`)
give every process a soft and a hard value for the sixteen resources of Linux (the numbers and the structure are Linux's), as POSIX says: the soft value is what is enforced and
anyone may move it anywhere up to the hard one, anyone may lower the hard value (and then cannot raise it again), only root may raise a hard value. They belong to the process: `fork`
copies them, `execve` keeps them, so the shell's `ulimit` reaches everything it starts. A process may look at, and change, another of the same user (any, as root: `prlimit`).

| Resource | Default (soft / hard) | Enforced |
|----------|----------------------|----------|
| `RLIMIT_NOFILE` | 1024 / 4096 | `fd_install` (open, pipe, socket, accept, `dup`, `F_DUPFD`) gives `EMFILE`, `dup2` beyond it `EBADF`, `F_DUPFD` from beyond it `EINVAL`, a Linux `poll` with more entries `EINVAL`; the table starts with 64 slots and grows on demand up to 4096 (`FD_HARD_MAX`), so no hard limit above 4096 (`EPERM`, root too). Until 2026-11-24 both values were 64 (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd") |
| `RLIMIT_NPROC` | 128 / 128 | `fork` gives `EAGAIN` when the *real user* already has that many processes (zombies count until collected); root is exempt. Half of the 256-slot process table: one user cannot fill it. Threads are not counted: they are limited per process (256) and in all (1024) |
| `RLIMIT_AS` | unlimited | every `mmap`, `brk` and other `uvm_map` of the running process fails with `ENOMEM` beyond the sum of its mappings (the 8 MiB stack counts at its initial 64 KiB) |
| `RLIMIT_DATA` | unlimited | the same call, for writable private mappings (heap, anonymous maps, the program's data and bss) |
| `RLIMIT_STACK` | 8 MiB / unlimited | the page fault handler refuses a stack page deeper than the limit: `SIGSEGV`. The stack region itself is 8 MiB, so a higher limit buys nothing |
| `RLIMIT_CORE` | 0 / unlimited | accepted: there are no core dumps |
| the rest (`CPU`, `FSIZE`, `RSS`, `MEMLOCK`, `LOCKS`, `SIGPENDING`, `MSGQUEUE`, `NICE`, `RTPRIO`, `RTTIME`) | as Linux reports them | stored and reported, not enforced |

`ulimit` in `gapsh` is a builtin (`-n -u -v -d -s -c -f -l -m -t -a`, `-S` and `-H`, `unlimited`, `hard` and `soft`; sizes in KiB; manual page `ulimit(1)`). The defaults leave every program
that ran before running. The Linux personality's `getrlimit`, `setrlimit` and `prlimit64` are the same calls (`NATIVE(SYS_getrlimit)` etc. in `kernel/linux/syscall.c`: the resource numbers, the structure and
the argument order are Linux's), so Linux programs see and meet the same limits; before the second round that table answered fixed values and accepted anything. Tests: `rlimtest` (`user/test`) and the shell
checks of `hardening2_limits`, `hardening2_fork_bomb_stops_at_the_limit`.

## The OOM killer

*Second round.* **The reserve.** The page allocator keeps the last 1/32 of RAM (at least 64, at most 2048 pages: 256 KiB to 8 MiB) for the kernel's own use. An allocation made *on behalf of a user
process*, `pmm_alloc_user`, fails while the free pages would drop under it: that is a page fault's page, the pages `uvm_populate` maps for `exec` and file maps, the copy `fork` makes of an
address space, the data pages of a tmpfs file, and the pages the Linux personality gives a shared anonymous mapping at the call (`lnx_mmap` in `kernel/linux/l3.c`). Everything else (page tables,
kernel stacks, the heap, DMA buffers: `pmm_alloc`) may use the reserve, which is what lets the kernel go on working long after the programs have hit the wall. **A new place that maps fresh memory into a
user address space must allocate with `oom_alloc_user_page` (or `pmm_alloc_user` where it cannot sleep)**: with a plain `pmm_alloc_page` a program could drive the free pages to nothing and the kernel's own
allocations would fail first. (`hardening2_oom_kills_a_linux_shared_memory_hog` tests `lnx_mmap`, which did that when main's L3 work arrived.)

**The design.** When a user allocation fails, `oom_alloc_user_page` (`kernel/mm/oom.c`) asks the killer once and tries once more:

* *Where the victim is chosen*: in the context of the allocation that failed (a page fault handler, a system call), never in an interrupt and never inside the allocator, whose irq-off
  sections only answer "no". The choice, the record and the signal are one step with interrupts off, so two failing allocations never end two processes for one shortage.
* *Whom*: every user process except init and except one already dying (`exiting`, or `SIGKILL` pending); kernel threads have no process. By badness: the RAM pages its tables map (a framebuffer
  mapping is not memory) plus the pages of *unlinked* tmpfs files it holds open (memory that dies with the last descriptor), times 5/4 for a process that runs as an ordinary user and 3/4 for root's,
  times 5/4 again when it started less than half a minute ago (the youngest lose the least work). The biggest wins; nothing with no pages is killed.
* *How, and when it takes effect*: `signal_send(victim, SIGKILL)`, exactly what `kill -9` does. The victim's threads die on their way back to user mode (a sleeping one is woken), and the last one out frees
  the address space and the descriptors in `process_finalize`; the killer frees nothing itself. The log line is `[Eiki] out of memory: killed 'NAME' (pid N), M pages`, `oom_kills` in `/proc/kstats` counts it,
  `/proc/oom` shows the last one.
* *The caller waits, and only where it is safe*: a caller that can sleep (it was interrupted with interrupts on: every system call, every user-mode fault) yields in 1 ms steps until the free pages
  are above the reserve again or the victim is a zombie, for at most 2 s, then tries the allocation once more. A page fault holds only its own address space's lock (the victim is another process, or
  the caller's own); a tmpfs write waits *before* it takes the file system lock (`tmp_write` calls `oom_wait_for_pages` when its attempt came up empty); the wait is bounded in any case. A caller that cannot
  sleep only starts the kill and fails.
* *When the victim is the caller* (or a thread of its process): the kill is sent and the caller does not wait for itself. A user-mode fault is let go (the pending `SIGKILL` ends the thread on the way out
  of the fault, before the instruction runs again); a copy to user space fails with `EFAULT`; either way the process is gone before it can run another instruction of its own.
* *One shortage, one victim*: while a victim is alive within 2 s of its kill, later callers wait for it instead of choosing another. A victim that has not died after that (it is stuck in something
  uninterruptible) is skipped as "dying already" and the next choice is made.
* *Kernel allocations never kill anybody*: a `kmalloc` or `pmm_alloc` that fails gives `ENOMEM` (or a short write) to its caller, whatever the cause.

`oom=off` on the command line keeps the reserve and the failing allocations but kills nobody (a fault ends in `SIGSEGV`, as before). Tests (`tests/case_hardening2.py`, guest program `oomtest`):
`hardening2_oom_kills_the_hog` (a program that maps and touches memory in a 128 MiB guest is ended, init, the shell and sshd live, the pages come back), `hardening2_oom_worker_survives_a_slow_hog`
(a hog that touches pages slowly while another program needs fresh memory all the time: the worker meets the shortage first, waits, and finishes with the right answers), `hardening2_oom_tmpfs_is_limited_as_a_whole`
(`ENOSPC` at half of RAM, nobody killed), `hardening2_oom_fork_of_a_big_process` (the shortage is met inside `fork`'s copy of the address space and the victim is a child),
`hardening2_oom_off_leaves_a_failing_fault`, and the fork bomb and descriptor leak in the limits tests.

**Not covered:** memory the kernel holds for a process that is not in its page tables (pipe and socket buffers, the heap objects behind its descriptors) is not counted in its badness and comes from the
kernel's side; a process that is stuck in an uninterruptible kernel wait cannot be killed until it gets out of it; the VM has no swap and no page cache to give back first. **Nothing but init is protected**:
sshd, the shell and the window server are ordinary candidates, and they live in the tests because the hog is the biggest and the youngest, not because they are special (a Linux `oom_score_adj` that exempts
chosen services is the next step).

## The lockup detector

*Second round.* `kernel/sched/watchdog.c` finds a task that has run **in the kernel** for too long **without ever giving the CPU up of its own accord** (a **soft lockup**). The kernel is preemptive
(the timer ends a time slice wherever the code is), so a spinning kernel thread does not stop the others: a count of context switches would say nothing, and what the detector counts instead is, per
task, the timer ticks that found it in the kernel (not in user mode, not the idle task) since it last slept, blocked, yielded, was seen in user mode, or was *touched* (`Task.kspin`, bumped by
`lockup_tick` from the tick's return path, cleared by `schedule()` when the task is no longer runnable, by `task_yield`, and by `lockup_touch()`). At `lockup.thresh` seconds of the task's own time (20 s
by default) it reports, with the tag Momiji (the hardware watchdog's):

```
[Momiji] soft lockup: 'NAME' (tid 7, pid 12) has run in the kernel for 20 s without sleeping, yielding or returning to user mode
rip=... <function+0x..>  cs=... (the registers of the interrupted context)
backtrace: ... (the panic code's symbolised walk of that context)
```

It counts in `watchdog_reports` and reports again after another threshold if the task is still at it. **`panic_on_lockup`** on the command line makes the report a panic instead (the usual incident text, the
record in RAM, and the restart that `panic=SECONDS` asks for: [RELIABILITY](RELIABILITY.md)). Command line words: `lockup.thresh=SECONDS` (1 to 3600), `lockup=off`, `panic_on_lockup`.

**Armed when the boot is complete** (`lockup_arm`, just before init starts): the boot thread does long work in the kernel (the check and repair of the volume under `hvfsck=safe`, unpacking the initrd) that the
hardware watchdog is fed through separately. **Long jobs that are alive** call `lockup_touch()`: the block layer does for every request (`blk_read`, `blk_write`, `blk_flush`), which covers the FAT and NTFS
scans and repairs at run time (a stick plugged in later). A user process spinning in user mode is never reported.

**Not covered: a hard lockup**, a CPU that runs with interrupts off for good. The timer interrupt does not come either, and QEMU has no NMI source to break in with. Today the TCO hardware watchdog ends it
(the machine restarts when nobody feeds it; the next start's record says "ended without a shutdown or a panic" with the last sign of life, but not why). The next step would be a performance-counter
NMI (a PC has one; QEMU's TCG does not emulate it) whose handler looks at the same counter and prints where the CPU is. Tests: `hardening2_lockup_is_reported`, `hardening2_lockup_panics_with_panic_on_lockup`,
`hardening2_lockup_off`, `hardening2_lockup_detector_stays_quiet` (the long legitimate jobs with a 3 s threshold and `panic_on_lockup`).

## Second round on real hardware: what to try on the Dell

None of the second round has run on the Dell (the i5-7500 has SMEP and SMAP; QEMU's `-cpu max` has both too). In this order, each with what turns it off:

1. **A clean start with the defaults.** `dmesg | grep -i -E 'smap|smep|Momiji'` must say `[Boundary] SMEP on` and `SMAP on`, and `[Momiji] soft lockup detector: ...`. A panic that names `SMAP VIOLATION` and an address
   means a path that QEMU never took touches user memory directly: note `rip` and the backtrace, and start again with **`smap=off`** (push with `--cmdline`; nothing else changes).
2. **A working session**: the desktop, an ssh login, the USB stick mounted and unmounted, sound, a `fetch`, `update status`. These exercise every driver the Dell has that QEMU does not (e1000e/I219, xHCI,
   HDA); none of them reaches user memory except through the system call layer, so a violation here would be new information.
3. **Limits**: `/usr/lib/kernel-tests/rlimtest` (all the rules, as an ordinary user), `/usr/lib/kernel-tests/oomtest fdleak`, `oomtest forkbomb`, `(ulimit -n 8; ls /usr/bin | head -3)`. A program that violates a limit should
   meet exactly what the table above says (`EMFILE`, `EAGAIN`, `ENOMEM`, `SIGSEGV` for the stack). There is no word that turns the limits off: the defaults only limit the descriptors (1024 soft, 4096 hard, Linux's usual values; 64 before 2026-11-24) and the processes
   of one user (128).
4. **The OOM killer** (only when you want to see it: the machine has gigabytes, and `oomtest hog` takes time and is unresponsive for a moment): `ulimit -v 1000000; oomtest hog` first (the address-space limit stops it
   with `mmap failed`, nothing is killed), then `oomtest hog` over ssh: `[Eiki] out of memory: killed 'oomtest' ...` in `dmesg`/the netconsole, the ssh session and the desktop live. **`oom=off`** keeps the reserve but kills nobody.
5. **The lockup detector**: add `crash=lockup lockup.thresh=10` for one start: the report (name `lockup`, the registers, a backtrace through `spin_forever`) appears about 10 s after boot completes, on the screen, in
   `dmesg`, over the netconsole and in `TOUHOU.LOG`; the machine keeps running (the spinner only takes its share of the CPU). With `panic_on_lockup panic=20` it is a panic and a restart after 20 s. Remove the words
   afterwards. **`lockup=off`** switches the detector off.

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
* **The second round** (`python tests/run_all.py hardening2_`, `tests/case_hardening2.py`; each test boots its own guest, the OOM ones with 128 MiB and sshd):

  | Test | What it shows |
  |------|---------------|
  | `hardening2_smap_on`, `_smap_off` | the start says SMAP and SMEP are on (or why SMAP is off with `smap=off`) and the system works in both |
  | `hardening2_crash_smap`, `_crash_smep`, `_crash_smap_window` | the three deliberate faults panic with `SMAP VIOLATION` / `SMEP VIOLATION` and the address; every copy routine closes its window |
  | `hardening2_ac_set_in_user_mode` | `acflag`: a program with `EFLAGS.AC` set lives through ticks and failing system calls, and the kernel never runs a handler with AC set |
  | `hardening2_limits` | `rlimtest` (defaults, the soft and hard rules, `EMFILE`, `EAGAIN`, `ENOMEM`, `SIGSEGV`, inheritance, `prlimit` on others) and the `ulimit` builtin |
  | `hardening2_fork_bomb_stops_at_the_limit` | `EAGAIN` at 128 processes (and at a lower `ulimit -u`), everything reaped, nobody killed |
  | `hardening2_oom_kills_the_hog` | the hog is ended (status 137, the log line, `oom_kills`), init, the shell and sshd keep their pids, the free pages come back |
  | `hardening2_oom_kills_a_threaded_hog` | a hog that grows in three threads is ended as one process (status 137): processes, tasks, user threads and kernel stacks are back at their old counts and the free pages come back |
  | `hardening2_oom_kills_a_linux_shared_memory_hog` | `lnx_shmhog` (`user/linuxtest`), a Linux program that maps shared anonymous memory (all its pages at the call) until stopped, is ended by the killer (137), its pages come back |
  | `hardening2_oom_worker_survives_a_slow_hog` | the program that needs fresh memory all the time waits for the victim and finishes with the right answers |
  | `hardening2_oom_fork_of_a_big_process` | `fork` copies every page, so a big process that keeps forking runs the machine out of memory inside `fork`: the killer ends children (the youngest of equal size), the parent lives and gets all 24 |
  | `hardening2_oom_tmpfs_is_limited_as_a_whole` | `ENOSPC` below half of RAM, no kill, the pages come back |
  | `hardening2_oom_off_leaves_a_failing_fault` | `oom=off`: `SIGSEGV` at the wall, `/proc/oom` says the killer is off |
  | `hardening2_lockup_is_reported`, `_panics_with_panic_on_lockup`, `_off` | `crash=lockup` with a 3 s threshold: the report, the panic, and silence with `lockup=off` |
  | `hardening2_lockup_detector_stays_quiet` | the runtime kernel jobs (a stick repaired by the automount thread, a slow repair, 60 MiB through the cache in 96 MiB of RAM, an NTFS scan, a big exec, the self tests) with a 3 s threshold and `panic_on_lockup`: no report |
* The safety net with SMAP on (the whole existing suite and the fuzzer): see "Results of the second round" below.

## Results of the second round (2026-11-17)

All in QEMU (TCG, `-cpu max`; the SMAP tests once more on `Skylake-Client-noTSX-IBRS`, the CPU model nearest the Dell's i5-7500). Nothing has run on the Dell.

The branch was merged with main while the work went on; the last four merges were `f4629f6` (the Linux `/proc` and exec-by-markers work), `7e0e727` (the demand-zero `.bss`), `06f5e56` (the L3 calls for Python
and gcc, ext4, a word-wise `memcpy`) and `98823c2` (the IPv6 stack). Each was audited for user-memory accesses by reading (none found: see "SMAP and SMEP") and the tests below were run on the merged tree.

* **The 19 `hardening2_` tests pass** (`python tests/run_all.py hardening2_`, about 4 minutes on an idle machine) on the final tree, merged with main `98823c2` (the panic_on_lockup test once failed on a greedy
  regular expression of the test itself; fixed). Mutation checks: with the `clac` taken
  out of `isr_common` the AC test fails with the kernel's own `[WARNING] EFLAGS.AC is set inside the handler of vector 32`, and with it taken out of `__uaccess_fault` `crash=smap-window` fails (the plain load after a faulting
  copy goes through): the two tests have teeth. So has the Linux shared-memory one: with `lnx_mmap` taking its pages from `pmm_alloc_page_zeroed()` again, `hardening2_oom_kills_a_linux_shared_memory_hog` fails
  (nobody is killed; the program spins on `ENOMEM` until the test gives up after 484 s), and with the fix it passes in 5 s.
* **The safety net with SMAP on**, all passing, nothing skipped within the lists, each group one run through `tools/qemu-slot.py` from a frozen copy of the tree. **No panic, no `[WARNING]`, no `SMAP VIOLATION`
  anywhere**: the audit's result (no user access had to be changed) held.
  * **On the final tree (main `98823c2`, the IPv6 stack)**: the 19 `hardening2_` tests, `selftest`, `utest` and the 5 `rig_` tests (26 in one run); the IPv6 and Linux socket tests, `net6_*` (16: the loopback,
    the five card models, host TCP, UDP and HTTP, DNS, address configuration, reassembly, neighbour discovery) and `linux_net*` (4, among them `linux_net6_matches_linux`); the 10 IPv4 socket tests that merge touched
    (`net_loopback`, `net_robust`, `net_loss`, `net_tcp_host`, `net_fetch_host`, `net_httpd_hostfwd`, `net_dns_host`, `net_ifconfig_set`, `net_dhcp_state_and_stop`, `net_static_config_at_boot`); all 16 `ssh_` tests;
    **all 7 `linux_py_*` tests** (L3: the real Python 3.12 and gcc 13 of an Ubuntu root run as Linux programs: the first program, the language and its library, the system calls, threads and multiprocessing,
    sockets and polling, the compiler, an interactive session; the multiprocessing script uses `lnx_mmap`'s shared memory); and the fuzzer, seeds 1, 2 and 201, 0 violations.
  * **On the tree merged with `06f5e56`**: the 19 `hardening2_` tests, `selftest`, `utest`, `kshell_serial`, `shell`, `crash_pf`, `crash_panic`, `desktop_boots`, `hardening_exec`, `rig_big_bss` and `manual`
    (29 in one run).
  * **On the tree merged with `7e0e727`** (the Linux `/proc` and exec-by-markers work, the demand-zero `.bss`): the 18 `hardening2_` tests; **all 33 `linux_` tests**, **all 16 `ssh_` tests** and 13 socket and netctl tests (`net_loopback`, `net_robust`, `net_loss`,
  `net_names_local`, `net_nonet_switch`, `net_tcp_host`, `net_fetch_host`, `net_httpd_hostfwd`, `net_dns_host`, `net_sntp_host`, `net_ifconfig_set`, `net_dhcp_state_and_stop`, `net_static_config_at_boot`);
  main's `rig_big_bss`; and 48 of the core, desktop and application tests: `selftest`, `utest`, `kshell_serial`, `kshell_keyboard`, `shell`, the nine `crash_*`, `hardening_exec`, `threads_utest`,
  `threads_selftest`, `jobctl` and its four variants, `desktop_boots`, `desktop_terminal`, `manual` (with the new `ulimit(1)`), `apps_cli`, `apps_windows`, the games (`games_selftests`, `minesweeper_play`,
  `snake_play`, `snake_scores`, `danmaku_menus`, `danmaku_demo`), `hidpi_*` (5), `lock_screen`, `login_*` (3), `pnote_*` (3), `pager`, `tooltip_*` (2). **The system call fuzzer: 14 campaigns of 30 s, 0 violations**:
  seeds 1, 2, 3 and 201 to 206 with two workers (9), 105, 111 and 112 with `--extra heapdebug` (3; seed 105 is the one that looked like a hang in the first round, Open, 1: it ran to its end this time), 301 and 302 with
  four workers (2). The demand-zero `.bss` (a fault taken inside the copy window, `rig_big_bss`) works under SMAP.
* **Not run on the final tree** (they ran on an earlier tree, or not at all): the other `linux_` tests (33 ran on the `7e0e727` tree; the L3 and IPv6 merges changed `kernel/linux/`), the kernel keyboard and
  desktop groups, the application and game tests (48 core, desktop and application tests ran on the `7e0e727` tree), `ext4_*` (a driver that never sees a user pointer), the network card models and the DHCP timing
  tests of IPv4 (`net_nic_*`, `net_link_*`, `net_e1000e_*`, `net_dhcp_*` other than the state one, `net_loader_netdiag`, `net_nicdebug_trace`, `net_wake_on_lan`), `tls_*`, the audio, explorer, storage and update
  tests. They are for the lead's whole-suite run (`tools/suite.py`, which an agent may not start). Nothing was run on the Dell.
* **What was measured** (128 MiB guest, 22759 free pages when idle): the reserve is 945 pages (3.7 MiB); the hog was ended with its 22412 pages (87.5 MiB) a few hundred milliseconds after it reached the wall and the free
  pages afterwards were exactly the 22759 from before; with a slow hog and a worker that needs 64 KiB of fresh memory per round, the worker ran 63131 rounds with no wrong answer; a tmpfs filler got 34 MiB in before `ENOSPC`
  (half of RAM, the 24 MiB initrd included); a 16 MiB process forking 24 children had children ended 4398 pages at a time while the parent got all 24; the lockup report named the task and `spin_forever+0x12` within the 3 s threshold.

## State when paused

**Merged into main on 2026-11-03** (`integration/2026-11-03p`). Reconciled with main: `kill(-pgid)` and `kill(-1)` use this branch's
`signal_group_from` (main's `signal_pgrp_checked` is gone; `accounts` holds both to the same EPERM rule), `listen()` uses `socket_ref()` and records the
listener's ids for `getpeercred`, main's `panic_begin()` replaces this branch's panic recursion guard, and `build.py` has the `test` kind
(`kerneltest=NAME`) next to `linuxtest`. On the merge: `hardening_exec` and `hardening_sysfuzz` (`TOUHOU_HARDENING_FUZZ=1`, three seeds) pass.
One decision of the merge: init (pid 1) drops every signal it has no handler for, from anybody, as on Linux, **except `SIGKILL` from root**: `kill -9 1` as root
still ends init and panics the machine (`signal_kill_init`), because the panic screen, the netconsole and the panic record are tested that way
(`hwaudit_panic_on_desktop`, `reliability_netcons_lines_and_panic`; docs/RELIABILITY.md). Nobody else can end init.

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
* **Not started** (at that pause; the first three were done in the second round of 2026-11-17, see "SMAP and SMEP", "Resource limits", "The OOM killer" and "The lockup detector"):
  * ~~Resource exhaustion~~, ~~SMAP~~, ~~the soft-lockup detector~~: done.
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

## How to resume

1. `git merge main`, `python tools/build.py`. Tests that wait for a shell prompt need `text` on their command line. Two QEMU runs in one checkout share `build/esp` (the harness writes
   `cmdline.txt` there), so run only one at a time, or copy the sources to another folder for a second one. The guest's clock runs slowly when the host is busy: wait for results, not for time.
2. Check the head first: `python tests/run_all.py crash,kshell,selftest,hardening_exec,jobctl,shell,utest,threads` (the `crash_*` tests exercise the rewritten backtrace), then the whole suite (about an hour).
3. Fuzz more: `python tools/fuzz.py --extra heapdebug --secs 30 105` (Open, 1), then `python tools/fuzz.py 201 202 203 204 205`, `--workers 4`, `--secs 120`, and `--root` (check the log for `UNKILLABLE`,
   `LEAK`, `INCIDENT`; after a panic replay with `--only N`). A failing run's logs are `build/fuzz-serial.log` and `build/fuzz-qemu.log`. Add every bug found to "What was found".
4. Write the init protection test (a guest program in `user/test/`, run as `kerneltest=initguard` or from a test shell as an ordinary user): `kill(1, SIGKILL)`, `kill(-1, SIGTERM)` from a throw-away session,
   `setpgid(0, 1)`, `ioctl(pty, TIOCSPGRP, &1)` followed by a Ctrl-\ through the master; afterwards `/proc/1/status` must still exist and no kernel warning must have been logged.
5. The remaining features:
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
   * (SMAP, resource exhaustion with the OOM killer and the soft-lockup detector were done in the second round, 2026-11-17: the sections above say how they work and what they do not cover.)
6. Failure injection: a `failalloc=N` command line word that makes every Nth `kmalloc` and page allocation fail, and a `sysfuzz` option that uses it; this is the way to reach the `ENOMEM` paths (Open, 4).
7. Where things are: `kernel/warn.c`, `kernel/mm/{heap,pmm,vmm,oom}.c`, `kernel/sched/watchdog.c`, `kernel/arch/x86_64/{backtrace.c,kprobe.asm,trap.c,cpu.c}`, `kernel/panic.c`, `kernel/printk.c`,
   `kernel/gfx/fbcon.c`, `kernel/drivers/fbdev.c`, `kernel/sys/{syscalls,exec,signal,uaccess,futex,thread,process}.c`, `kernel/fs/{tmpfs,vfs,pipe,socket}.c`, `kernel/drivers/{tty,pty}.c`, `user/test/*`,
   `user/sbin/init.c` (the `kerneltest=` block), `tools/fuzz.py`, `tests/case_hardening.py`.
