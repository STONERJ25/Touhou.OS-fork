# Threads (Alice strings several puppets from one shrine)

POSIX-style threads: a kernel `clone`-like call, a futex, per-thread TLS (through the FS base), per-thread signal masks and
a pthread library on top. This page says plainly what exists and what does not; the "Not done" list at the end is the
honest backlog.

## Status

| Done | Where |
|------|-------|
| `thread_create` / `thread_exit` / `exit_group` / `gettid` / `tgkill` / `set_tls` / `sigsuspend` / `sigtimedwait` / `sigpending` system calls | `kernel/sys/thread.c` |
| deferred `pthread_cancel` (cancellation points, cleanup handlers, enable/disable), `sigsuspend`, `sigwait`, `sigtimedwait`, `sigpending` | `signal.c`, `pthread.c`, `sys.c` |
| futex (wait with timeout, wake, cmp-requeue), keyed by physical address | `kernel/sys/futex.c` |
| one `Task` per thread inside a shared `Process`; FS base saved/loaded on context switch | `kernel/sched/task.c` |
| per-thread signal mask and thread-directed pending set; process-directed signals go to a thread that does not block them | `kernel/sys/signal.c` |
| group exit (`exit`, fatal signals, SIGSEGV in any thread), last thread out tears the process down | `kernel/sys/process.c` |
| `execve` kills the other threads and waits until they have left the kernel | `kernel/sys/exec.c`, `thread_kill_others` |
| `fork` in a threaded process keeps only the caller (TLS base and signal mask carried over) | `kernel/sys/process.c` |
| address-space lock (mmap/munmap/mprotect/brk, fork's copy, page faults), descriptor-table lock, cwd ring | `kernel/sys/syscalls.c`, `uvm.c`, `process.c` |
| limits: 256 threads per process, 1024 in the system, `EAGAIN` beyond that | `shared/syscall.h` |
| `/proc/<pid>/threads`, `Threads:` in `/proc/<pid>/status`, `pid=`/`tid=` on `/proc/tasks`, `ps -T` | `kernel/fs/procfs.c`, `kappabox/sys.c` |
| libc: pthread create/join/detach/exit/self/equal/kill/sigmask, attributes (stack size, guard, detach state) | `user/libc/src/pthread.c` |
| libc: mutexes (normal, recursive, errorcheck, trylock, timedlock), condition variables (wait, timedwait, signal, broadcast), rwlocks, barriers, spin locks, `pthread_once`, thread-specific data with destructors, cleanup handlers, `pthread_atfork` | same |
| libc: POSIX semaphores (`sem_*`), `sched_yield`, `gettid` | same |
| libc: `_Thread_local` / `__thread` (static TLS: `PT_TLS` segment in `user.ld`, one block per thread), C11 `<threads.h>` | `user/user.ld`, `pthread.c`, `cthreads.c`, `threads.h` |
| libc: per-thread `errno`; thread-safe `malloc`/`free`/`realloc` and stdio; `fork()` holds both locks across the call | `errno.h`, `stdlib.c`, `stdio.c`, `sys.c` |
| guard page below every thread stack (a stack overflow is a SIGSEGV that ends the process) | `pthread_create` |
| tests: `utest` thread section (17 parts), kernel self test for the futex queues, `tests/case_threads.py` | `user/bin/utest_threads.inc`, `kernel/sys/futex.c` |

## The model

* A **`Process`** is the thread group: address space, descriptor table, cwd, umask, credentials, signal *dispositions*, the
  process-directed pending set, the parent/child links. It lists its threads in `Process.threads` (guarded by interrupt
  disabling, the lock of a one-CPU kernel) and counts them in `nthreads`.
* A **`Task`** that belongs to a process is one **thread**: kernel stack, saved FPU state, `fs_base`, `sig_blocked` (its
  signal mask), `sig_pending` (signals aimed at this thread), `clear_tid`, and `utid`, the id `gettid()` returns. The
  first thread's id is the pid; other threads draw ids from the same counter, so a thread id never equals another
  process's pid. `Process.task` still names one live thread for older code.
* The scheduler is unchanged round robin over all tasks; each thread has its own slice. On a switch to a user thread
  whose `fs_base` differs from what `IA32_FS_BASE` holds, the MSR is written (the kernel itself never uses FS, and user
  mode cannot change it, because `CR4.FSGSBASE` stays off).

## System calls (`shared/syscall.h`, numbers 106-115)

| call | meaning |
|------|---------|
| `thread_create(flags=0, stack_top, tls_base, tid_addr, entry, arg)` -> tid | new thread starts at `entry(arg)` with `rsp = stack_top`, FS base `tls_base`, the creator's signal mask and FPU state. The kernel stores the tid at `*tid_addr` before the thread can run and, when the thread exits, zeroes that word and wakes its futex waiters (that is how join works). `-EAGAIN` over the limits |
| `thread_exit(status)` | ends the calling thread only. The last thread to leave ends the process (exit status 0, or whatever `exit_group` chose) |
| `exit_group(status)` / `exit(status)` | `SYS_exit` keeps meaning "end the process", so every old program is correct unchanged |
| `gettid()` | thread id |
| `tgkill(pid, tid, sig)` | signal one thread (used by `pthread_kill` and `raise`) |
| `set_tls(base)` | sets the caller's FS base (the main thread's startup uses it) |
| `futex(uaddr, op, val, timeout, uaddr2, val3)` | `FUTEX_WAIT` (sleep while `*uaddr == val`; relative timeout; `-EAGAIN`, `-ETIMEDOUT`, `-EINTR`), `FUTEX_WAKE`, `FUTEX_CMP_REQUEUE` |

### The futex

A waiter checks the word and joins the FIFO in one interrupt-disabled section, so a wake-up that follows a change of the
word cannot be missed. The key is the *physical* address of the word, which makes private words and words in shared
mappings (`MAP_SHARED` tmpfs files) behave the same. A page that is not mapped yet is faulted in for a waiter; a waker
whose page is absent wakes nobody. Waiters are woken oldest first; a timed-out or interrupted waiter removes itself.

### Exit, exec, fork

* **Group exit.** `process_exit` (also the default action of a fatal signal, so a SIGSEGV in any thread ends the whole
  process) sets `Process.exiting`, records the status (first caller wins) and wakes every other thread out of
  interruptible sleeps. Blocking calls ask `signal_pending_now()`, which also answers "this thread must die", so they
  return `-EINTR`; on the way out of the kernel each thread notices the flag and leaves through `thread_leave`. Threads
  in uninterruptible kernel waits leave when those finish.
* **`thread_leave`** zeroes and wakes `clear_tid`, unlinks the thread, wakes `thread_wq`, and if it was the last thread
  runs `process_finalize` (close descriptors, drop cwd, free the address space, reparent children, zombie, SIGCHLD).
  `wait` therefore only reports the process once *all* threads are gone.
* **`execve`** builds the new image first (an error leaves every thread alive). At the point of no return it sets
  `Process.execing`, wakes the other threads, and sleeps on `thread_wq` until `nthreads == 1`: a thread asleep in
  `read()` unwinds through its syscall exit path (freeing its buffers and file references) before the old address space
  is destroyed. The survivor becomes the first thread (`gettid() == getpid()`), its TLS base is cleared. If another thread
  already started an exit or exec, the caller leaves instead.
* **`fork`** copies the address space of the whole process but creates one thread in the child, a copy of the caller (TLS
  base and signal mask included). The libc wrapper holds the allocator and stdio locks across the call and releases them in
  both processes, so `malloc` and `printf` work in the child.

### Signals

* Dispositions are per process; the mask and the thread-directed pending set are per thread. `sigprocmask` is
  per thread. A signal handler runs on the thread that takes the signal and blocks the signal for its duration.
* `kill` / terminal signals / SIGCHLD are process-directed: they pend on the process and `signal_send` wakes one thread
  that does not block them (the running thread when it qualifies). Whichever eligible thread passes through
  `signal_deliver` first takes the signal. If every thread blocks it, it stays pending until one unblocks it.
* `tgkill`, `pthread_kill`, `raise` and synchronous faults (page fault, #UD, #DE, ...) are thread-directed. A fault forces
  the default action, which ends the process.
* A process-directed signal can interrupt a thread that another thread then beats to the delivery; that thread sees a
  spurious `EINTR`. All libc loops handle `EINTR`.

### Locking (what protects what)

The kernel has one CPU, so "a lock" is either interrupt disabling (short sections) or a sleeping `Mutex`.

| data | lock |
|------|------|
| `Process.threads`, `nthreads`, thread-directed and process pending sets, futex FIFO, descriptor table slots | interrupts disabled (`irq_save`) |
| VMA list, page tables below `pml4`, `brk`, `mmap_next` | `AddrSpace.lock` (sleeping mutex): mmap, munmap, mprotect, brk, fork's copy and `uvm_fault` take it. A fault taken with interrupts on sleeps on it; one taken with interrupts off fails the access instead |
| descriptors | `fd_install`/`fd_close`/`fd_get_ref` are atomic; `read` and `write` hold a reference, so a close in another thread cannot free the `File` under them. Other calls still use the borrowed pointer of `fd_get` |
| cwd | `chdir` in a threaded process keeps the old directory referenced (8 entries, released at exit): another thread may still be resolving a path against it |
| credentials, umask, signal dispositions | plain word writes; two threads changing them at the same instant is the caller's race |

## libc

* `errno` is `(*__errno_location())`, a word in the thread's control block, found through `%fs:0` (the control block
  starts with a pointer to itself, as the x86-64 TLS ABI wants, then `errno`). Startup (`__libc_start`) maps the main
  thread's TLS block and control block and calls `set_tls` before anything can fail.
* **Static TLS** (`_Thread_local`, `__thread`, `thread_local`): `user/user.ld` defines a `PT_TLS` segment from `.tdata` and
  `.tbss` and exports `__tls_init_image/size`, `__tls_mem_size`, `__tls_align`. Each thread gets a block directly below its
  control block (x86-64 variant II, local-exec model: the linker bakes negative offsets from the thread pointer into the
  code); `.tdata` is copied in, `.tbss` is the zeroed rest of a fresh mapping. The block is placed with the linker's own
  formula, so any alignment the linker accepts works. There is no dynamic loader, so no other TLS models exist.
* `pthread_create` maps `guard + stack + control block` in one block (default stack 256 KiB, 4 KiB guard, `PROT_NONE`);
  the control block is at the top, the stack grows down below it. The new thread starts on that stack through
  `thread_create`, so there is no assembly stub. `pthread_join` waits on the thread's tid word and then unmaps the block.
  A detached thread queues its block on a list that later `pthread_create` calls unmap once the kernel has zeroed the word.
* Mutexes are the classic 0/1/2 futex mutex; recursive and errorcheck kinds track the owner's tid. Condition variables use
  a sequence number: the waiter reads it before releasing the mutex and sleeps only while it is unchanged, so a signal
  between the unlock and the sleep is never lost. Timeouts are absolute `CLOCK_REALTIME` times converted to a relative
  futex timeout. Spurious wake-ups are possible (as POSIX allows).
* `malloc`/`free`/`realloc` share one lock (`__malloc_lock`). stdio takes one recursive lock for every entry point
  (`__stdio_lock`), so a `printf` is never interleaved with another thread's output.
* **Audit of globals.** Not thread-safe by design, documented here: `strtok`, `asctime`/`ctime`/`localtime`/`gmtime`
  (static result), `getpwnam`/`getpwuid`/`getgrnam`/`getgrgid` and the shadow lookups (static result buffers), `getopt`,
  `tmpnam(NULL)`, `rand`/`srand`, `setenv`/`putenv`/`unsetenv`, `readdir` (the `DIR` is per stream; sharing one `DIR` between
  threads is the caller's problem). The `_r` variants that exist (`strtok_r`, `localtime_r`, `gmtime_r`) use no shared
  state except the time-zone cache: call `tzset()` before starting threads.

## Tests

* `python tests/run_all.py threads` runs `tests/case_threads.py`: the thread section of `utest` and the kernel self test.
* `utest` (`user/bin/utest_threads.inc`) covers: create/join with return values, 8 threads on a mutex counter and an
  atomic counter, producer/consumer on condition variables, rwlock, barrier, errno isolation, thread-specific data with
  destructors, `pthread_once`, detached threads, 200 threads created and joined (and 100 alive at once), `/proc`,
  `pthread_kill` (the handler runs in the aimed thread), process-directed signals landing on the thread that does not
  block them, `raise`, futex/condvar/semaphore/mutex timeouts, recursive and errorcheck mutexes, the thread limit
  (`EAGAIN`, slots come back), SIGSEGV in a thread ending the process, fork from a thread, exec from a thread while another
  thread sleeps, `exit()` while threads block in `read`, `poll`, a futex, `sleep` and a condition variable, `exit_group`
  from a non-main thread, the last thread ending the process with status 0, and a stress loop (threads + pipes +
  mmap/munmap + fork) that compares free memory before and after.
* The kernel self test (`selftest` on the command line) checks the futex queues: no-waiter wake, value mismatch, timeout,
  oldest-first wake order, requeue.

## Cancellation, sigsuspend and sigwait

* `pthread_cancel(t)` sets a flag in the target's control block and sends it `SIGCANCEL` (31) with `tgkill`. The kernel
  treats that signal as *sticky*: `signal_deliver` never runs a handler for it, it stays pending, and any interruptible
  wait of that thread returns, after which `syscall_dispatch` reports `-ECANCELED` instead of `-EINTR`. libc's `__ret()`
  turns `-ECANCELED` into `pthread_exit(PTHREAD_CANCELED)`, so every wrapper that can block (read, write, poll, nanosleep,
  sleep, waitpid, pause, sockets, ...) is a cancellation point; `pthread_join`, `pthread_cond_wait/timedwait` (the mutex is
  re-acquired before the cleanup handlers run) and `sem_wait/timedwait` are too, through `FUTEX_CANCELLABLE`. A futex wait
  without that flag (mutex locks) ignores a pending cancel, so it does not spin. `pthread_testcancel` checks the flag.
* A thread disables cancellation by *blocking* SIGCANCEL (`pthread_setcancelstate`); libc keeps the bit out of every mask
  an application can see or set (`sigprocmask`), and a new thread starts with it enabled. Cleanup handlers
  (`pthread_cleanup_push/pop`), thread-specific-data destructors and `join` all work for a cancelled thread.
  `PTHREAD_CANCEL_ASYNCHRONOUS` is accepted and behaves as deferred. stdio functions are cancellation points only through
  `read`/`write`; a thread cancelled inside stdio releases the stdio lock on its way out.
* `sigsuspend(mask)` (113) swaps the calling thread's mask and sleeps; the handler runs under the temporary mask and
  `sigreturn` restores the original (`Task.saved_mask`). `sigtimedwait`/`sigwait` (114) accept a pending signal of the set
  without a handler; a thread waiting for a signal that every thread blocks is woken by it (`Task.sigwait_set`).
  `sigpending` (115) reports pending-and-blocked signals of the thread and the process. `pause()` works per thread.

## Not done

* **Kernel stack reuse:** user thread blocks of the default size are cached by libc (8 entries) and reused, but kernel
  stacks still take fresh virtual addresses (`kstack_alloc` in `kernel/mm/vmm.c` never recycles them); their page-table
  pages stay until reboot, about one 4 KiB page per 100 threads ever created. Reusing the addresses needs an explicit
  `invlpg` in `kstack_free` (the shared kernel mappings are not flushed when the current CR3 is a user one). No pages leak
  after a process ends; the stress test allows 1 MiB of slack and measured under 100 KiB before the cache existed.
* **Descriptor races:** only `read`/`write` hold a reference across the call; closing a descriptor in one thread while
  another uses it in `ioctl`, `poll`, `lseek`, ... can still touch a freed `File`.
* **Locks are not SMP-ready:** they are interrupt-disabling sections and sleeping mutexes; a port to several CPUs must
  replace the former with real spin locks (every such section is short and marked by `irq_save`).
* A thread stuck in an uninterruptible kernel wait delays a group exit or an `execve` until that wait ends.
* Async cancellation, robust mutexes, priority inheritance, process-shared attributes (the futex itself is shared-memory
  capable, the attributes are not parsed) and thread names are not provided.

## State when paused

Paused on 2026-10-23 at the user's request. The branch head builds and its tree is the one that was tested.

* **Done and tested:** everything in the Status table. Last complete run (`python tests/run_all.py`, commit `a279b04`):
  `35 tests, 0 failed`. After that only the thread-block cache and a `ps -T` row check in `utest` were added; for those
  `python tests/run_all.py utest` (which also runs `threads_utest`) passed. The full suite was not re-run after them.
* **Half done (parked, not in the tree):** per-stream stdio locks. Today one recursive lock (`__stdio_lock`) covers all
  streams, so a thread blocked in `fgets(stdin)` holds it and stops every other thread from printing, and `exit()` called
  while another thread is blocked in `read` on a stream waits for it. The unfinished edit is commit `8493a55` (it does
  not build; `bb51167` reverts it). What is left is listed in that commit message. Resume with
  `git show 8493a55 -- user/libc/src/stdio.c`, finish the wrappers, then run `python tests/run_all.py utest` and `shell`.
* **Not started:** kernel stack address reuse (see "Not done"), `fd_get_ref` in the remaining descriptor syscalls,
  `pthread_rwlock_timed*`, `pthread_getattr_np`, a timeout on the exec wait for a thread stuck in an uninterruptible wait.
* **Known problems:** (1) the global stdio lock above. (2) The QEMU harness can lose the last serial bytes when the guest
  powers off right after the verdict (seen once in a full run: `QEMU exited (code 1)` with truncated output, the guest
  had actually finished); `tests/case_threads.py` repeats a run that ends without a verdict, other cases do not.
  (3) A spurious `EINTR` is possible when two threads race to take a process-directed signal.
* **How to resume:** `python tools/build.py`, then `python tests/run_all.py threads` (thread section of `utest` plus the
  futex self test, about 15 s), then `python tests/run_all.py utest` and `shell` for the libc and userland regressions.
  The tests in `user/bin/utest_threads.inc` are the specification of what must keep working; add a section there and to
  `THREAD_SECTIONS` in `tests/case_threads.py` for anything new.
