# Copy-on-write fork

`fork` used to copy every page of the parent before it returned. Since 2026-11-24 it shares the pages and copies one only when
somebody writes to it. The code is `uvm_clone`, `uvm_cow_break` and the present-page branch of `uvm_fault_locked` in
`kernel/sys/uvm.c`, plus one rule in `kernel/sys/futex.c`. Sources: the Intel SDM vol. 3A (4.5 paging, 4.6 access rights and
`CR0.WP`, 4.7 the page-fault error code, 4.10.4 `INVLPG`), POSIX `fork`, and the design below. No code from other systems.

The kernel command-line word **`cow=off`** keeps the eager copy (`cow=on` asks for copy-on-write; the default is
`UVM_COW_DEFAULT` in `kernel/include/process.h`, which is 1: on). The word is read once, at the first fork.

## Measurements (QEMU, `forkbench`, 512 MiB guest; `tests/case_cow.py` prints them on every run)

| | eager copy (`cow=off`, the kernel before this change) | copy-on-write |
|---|---|---|
| fork + wait, 1 MiB touched | 1.0 ms | 0.25 ms |
| fork + wait, 16 MiB touched | 9.95 ms | 1.1 ms |
| fork + wait, 64 MiB touched | 21.4 ms | 4.2 ms |
| free memory a living child costs, 1 / 16 / 64 MiB parent | 1260 / 16656 / 65908 kB | 72 / 104 / 204 kB |
| 200 x fork + exec `/bin/true` (a small parent) | 565 ms | 442 ms |
| fork of a process holding 60% of the free memory | fails (`ENOMEM`) or the OOM killer | works |

(The guest's clock ticks in milliseconds, so the small numbers are coarse. 100 x `sh -c true` on the Linux root was not
measured.) What is left of the cost of a fork is one walk over the parent's mappings and the child's page tables: about 65 us
per MiB here.

## The page-table entry

A page that is shared because of a fork has, in **every** address space that maps it:

* `PTE_W` clear, whatever the mapping allows;
* `PTE_COW` set (bit 9, one of the three bits the CPU ignores in a present entry);
* a reference in `Page.refcount` for each entry that points at it (`pmm_get_page` at fork, `pmm_free_page` drops one and frees
  the page at the last).

The mapping's rights stay in the VMA (`Vma.prot`): the entry says "read-only for now", the VMA says what a write should get.
An entry is never both `PTE_W` and `PTE_COW`.

## What fork shares

`uvm_clone` walks the parent's VMAs with the parent's memory lock held:

| Page | In the child |
|---|---|
| in a `VMA_IO` mapping (device memory, a tmpfs file mapped `MAP_SHARED`, the Linux personality's `MAP_SHARED\|MAP_ANONYMOUS`) | the same frame, same rights, shared for real, never marked (as before) |
| present in a private mapping (data, heap, stack, anonymous `mmap`, a private file mapping, also a read-only one such as the program text) | the same frame, read-only and marked in **both** parent and child, refcount + 1 |
| not present (demand-zero memory nobody touched) | not present: each side gets its own zero page when it touches it |
| a private page with a futex waiter on it (see below) | an eager copy; the parent's entry is untouched |
| `cow=off` | an eager copy of every private page |

Read-only pages are marked too so that a later `mprotect(PROT_WRITE)` finds the mark. The parent's entries change under its
feet, so each one is invalidated (`INVLPG` in `vmm_map`, because the parent's tables are the loaded ones). **One CPU runs
kernel and user code** (the application processors are not started), so there is no other TLB to reach; a kernel that starts
them needs a shootdown here and in `uvm_cow_break`.

Both personalities come through `process_fork_ex`: native `fork`, Linux `fork`, `vfork` (still a fork) and `clone` without
`CLONE_VM`. Threads (`CLONE_VM`, the native thread calls) share one `AddrSpace` and one set of tables: nothing is
copy-on-write among them.

## The write fault

A write to a shared page faults with "present, write" (error code bits 0 and 1), from user mode or from the kernel.
`uvm_fault_locked`, with the address space's lock held:

1. no VMA, a `VMA_IO` mapping, an instruction fetch or a reserved-bit fault: not ours;
2. the entry is writable by now: another thread of the process broke the sharing while this one waited for the lock: retry;
3. the entry is not marked, or the VMA does not allow writing: a real protection fault (`SIGSEGV`, or `-EFAULT` for the kernel);
4. `uvm_cow_break`: if the reference count is 1 (everybody else exited, exec'ed, unmapped or took a copy) the entry becomes
   an ordinary one **in place**; otherwise a page comes from `oom_alloc_user_page`, gets the contents, is mapped with the
   VMA's rights, and the old page loses one reference. `vmm_map` invalidates the TLB entry. The allocation may wait for the
   OOM killer, so the entry is read again after it.

Two processes that break the same page at the same time each hold their own lock; the reference count is only changed with
interrupts off (`pmm.c`). The worst case is one copy more than needed (both saw 2), never a page written by two owners:
"refcount 1" can only become 2 again through a fork of the address space whose lock the breaker holds.

### Kernel writes to user memory

`CR0.WP` is set (`cpu.c`), so a supervisor write to a read-only page faults like a user one. The fault handler's order
(`trap.c`, unchanged): SMEP check, SMAP check (a present user page touched with `EFLAGS.AC` clear is a panic: the copy
routines of `uaccess.asm` set AC, so their faults pass), then `process_pagefault`, which offers the fault to `uvm_fault`
**before** it redirects the copy routine to its `-EFAULT` exit. So every kernel write through the window breaks the sharing
and the `rep movsb` continues. The list of ways the kernel writes user memory, and how each is covered:

| Path | Covered by |
|---|---|
| `copy_to_user`, `clear_user` (every system call that returns data: `read`, `clock_gettime`, `wait`, `stat`, signal frames, the Linux personality's results, `clear_tid` at thread exit) | the write fault, as above (`cow_test kernel_writes`: `read()` and `clock_gettime` into a shared page in the child and in the parent) |
| `uvm_write` (through the direct map, which has no rights): fork's `settid` word in the **child**, the exec stack, the Linux stack image | `uvm_write` calls `uvm_cow_break` for each page first; exec and the Linux stack write into a new address space whose pages are fresh |
| exec's segment loader and the private file `mmap` (`vfs_read` into the direct map) | the pages were allocated a moment before (after `uvm_unmap` for `MAP_FIXED`): never shared |
| futex words | the kernel only reads them; see below for the key |
| `/proc/<pid>/cmdline`, `environ` | reads only |

Not covered because it does not exist: writing another process's memory (`ptrace`, a writable `/proc/<pid>/mem`). Whoever adds
it must call `uvm_cow_break` under that process's lock.

A kernel write with interrupts off whose lock is taken, or whose copy cannot be allocated, ends as `-EFAULT` from the copy
routine (the same as for a demand-zero page before).

### Futexes

The futex wait list knows a word by its **physical address**. A shared page has one address in several processes, and the
address changes for the process that writes first: a thread asleep on the old page would never hear a wake-up sent to the new
one. Rule: **a page with a waiter is never shared.**

* a waiter (`futex_key` with populate) breaks the sharing of its page before it takes the address, and checks with interrupts
  off, just before it joins the list, that the entry is still that page and unmarked (a sibling thread may have forked in
  between; then it starts over). The same for both words of a requeue;
* fork copies a page that has a waiter eagerly (`futex_page_has_waiters`), so the parent's entry stays writable and its own;
* a waker that finds the page still marked has nobody to wake and returns 0.

`cow_test threads_and_futex` forks while a thread sleeps in `pthread_cond_wait` on the page and then wakes it.

## The other calls

* **`mprotect`** (`uvm_protect`) and **`mremap`** when it moves pages: the new entry is made by `uvm_keep_cow`, which keeps a
  marked entry marked and read-only. Before this, both rewrote the entry from the VMA's rights, which would have made a shared
  page writable.
* **`munmap`, `madvise(MADV_DONTNEED)`, `brk` shrinking, `exit`, `exec`**: each entry that goes drops one reference
  (`pmm_free_page`); the page is freed at the last. A parent whose child has exec'ed or exited finds refcount 1 at its next
  write and keeps the page (one fault, no copy).
* **Lock order**: the fault handler takes `AddrSpace.lock` and nothing else that sleeps except the allocator. No code may touch
  user memory through the uaccess routines while it holds the same address space's lock (the fault would wait for itself);
  this was already the rule for demand paging and the holders (`mmap`/`munmap`/`mprotect`/`brk`, `madvise`, `mremap`, fork)
  obey it. Fork holds only the parent's lock; the child is not visible yet.

## Accounting

* `/proc/<pid>/status` and `stat` (RSS), the Linux `/proc` and the OOM killer's badness count the pages an address space
  **maps**: a shared page counts fully in each process (no `Pss`). The sum of all RSS can exceed the RAM.
* `VmSize`, `RLIMIT_AS` and `RLIMIT_DATA` count mappings, not pages: unchanged.
* Memory is **not reserved at fork**: a page is charged (allocated) when it is copied. Fork therefore overcommits: a parent
  and a child that both write everything need twice the memory, and the one that cannot get a page meets the OOM path of a
  write fault: the killer picks a victim (`oom.c`); if nothing can be freed the writer gets `SIGSEGV`; a kernel copy returns
  `-EFAULT`. Never a panic. `cow_fork_under_memory_pressure` does exactly this.
* `hardening2_oom_fork_of_a_big_process` tested that the OOM killer strikes **inside** fork's eager copy; its children only
  sleep, so with copy-on-write nothing runs out. That test now boots with `cow=off` (it still covers the eager path).

## Tests

`python tools/qemu-slot.py -- python tests/run_all.py cow_` (`tests/case_cow.py`; guest programs `user/test/cow_test.c`,
`user/test/forkbench.c`): what each side sees after writes by child, parent, grandchild and eight siblings on data, bss,
heap, stack and anonymous memory; kernel writes; fork with a futex waiter; `MAP_SHARED` stays shared; `mprotect` and a
`munmap` in the middle; 100 fork+exec; 200 forks without a drift of the free memory; the cost of a living child (fails
without copy-on-write); the same with `cow=off`; fork under memory pressure (impossible without); forkbench both ways.

## Not done

* A real `vfork` (`LNX_vfork` and `clone(CLONE_VFORK|CLONE_VM)` are still a fork, now a cheap one).
* A shared zero page for reads of untouched memory; sharing of page-table pages; `Pss`.
* Reserving memory at fork (strict accounting), `MADV_DONTFORK`/`MADV_WIPEONFORK` (accepted and ignored, as before).
* More than one CPU: TLB shootdown, and an atomic test of the reference count in `uvm_cow_break`.
