# Running Linux (and later Windows) programs: the plan

**Goal, very far out:** run unmodified Linux programs on TouhouOS, and after them Windows programs, with Windows-era games as the benchmark. This page is the plan for the Linux half (Windows comes after it and will get its own page), the rules the work follows, and what was measured before any kernel code was touched.

## The rule: original code, from specifications and from behaviour

The project's other large parts (TLS, TrueType, the web engine, the JavaScript engine, the C compiler) were written from public specifications with no code taken from other projects. The compatibility layers follow the same rule, on purpose, even though it costs more: a ground-up layer fits the kernel it lives in, and the kernel is still young enough to shape both together.

* **Allowed sources:** public specifications and documentation (POSIX; the Linux man pages as documentation; the System V x86-64 psABI; the ELF specification; the Intel and AMD manuals; the PE/COFF specification and Microsoft's public API documentation), and the **observed behaviour** of real systems, measured with our own test programs (a Linux under WSL on the development PC; the owner's Windows).
* **Not allowed:** reading or copying the source of the Linux kernel, glibc, musl, the BSDs, Wine, ReactOS, Mesa, SDL or any other implementation of what we build.
* **Test inputs are not code we ship.** Third-party Linux programs (a busybox, Python, a distribution's `ls`) are inputs to tests, never part of the tree. A dynamic glibc or musl program needs *its own* dynamic linker and libc files at the paths it names: those are data the user provides in a Linux root directory, not something we write or ship.
* **Struct layouts, constants and numbers** come from the psABI and the man pages and are *verified by oracle programs*: a C program built on a real Linux prints `sizeof`/`offsetof`/values, and its output is stored as a test vector (facts, not code).
* **Behaviour is pinned by differential tests:** the same small program runs on real Linux (WSL) and on TouhouOS under the Linux personality and the outputs (results, `errno` values, edge cases) must be identical. The tests are the specification.

## What a Linux program asks of a kernel (measured)

`tools/linuxcensus.c` is a small `ptrace` counter we wrote (no `strace` is needed) and `tools/linuxcensus.py` runs it over a set of programs inside WSL: `wsl.exe -e python3 tools/linuxcensus.py`. The first run (Ubuntu 24.04 in WSL, kernel 6.18) covered nine programs: `true`, `ls -l`, `cat`, a `sh` pipeline, Python doing nothing, Python with threads, sockets and hashing, a `gcc -c`, a static glibc hello, and a hello that makes raw system calls without any libc.

* **60 different system calls in all.** A program with no libc needs **3** (`execve`, `write`, `exit_group`); a static glibc hello needs **17**; `ls`, `cat` and a shell need about 40; Python and gcc need a few more (`clone3`, `epoll_create1`, `sockets`, `vfork`, `wait4`, `pipe2`, `getrusage`).
* The ten calls **every** program makes before its first line of code: `execve`, `brk`, `arch_prctl(ARCH_SET_FS)`, `set_tid_address`, `set_robust_list`, `rseq`, `prlimit64`, `mprotect`, `fstat`, `exit_group`; then `openat`, `mmap`, `read`, `pread64`, `close`, `access` for the dynamic loader.
* The flag values that matter: `mmap` is `PRIVATE|FIXED|DENYWRITE` (the loader mapping libraries), `PRIVATE|ANONYMOUS`, `SHARED`; `clone` with `CHILD_CLEARTID|CHILD_SETTID` (fork-like) and `clone3` for threads; `futex` `WAKE|PRIVATE` and `WAIT_BITSET`; `ioctl` `TCGETS` only. `rseq` and `set_robust_list` can be refused (glibc copes); `lgetxattr`/`listxattr` can answer "not supported".
* The full table is `build/linuxcensus.md` after a run.

## Where TouhouOS stands (read from the code, 2026-11-03)

* About 100 native system calls with **our own numbers and structures**. They stay: they are the native ABI (`shared/syscall.h`).
* The kernel's loader takes only **static, fixed-address ELF files** (`ET_EXEC`, `PT_LOAD`): no `PT_INTERP`, no position-independent executables, no `PT_TLS`, no auxiliary vector.
* Present and useful: `futex`, `mmap`/`mprotect`/`brk`, `fork`/`execve`/`waitpid`, pipes, ptys, sockets, `procfs`, signals (our own layout), per-thread FS base (`set_tls`), FXSAVE state per task (SSE2; no AVX: `OSXSAVE` is off, so a libc will pick its SSE2 paths), a 47-bit user address space.
* Missing for Linux: the `*at` family and `dirfd` paths, `clone` with Linux's flag semantics (thread groups, `SETTLS`, `CHILD_CLEARTID` with the futex wake), `vfork`, Linux's signal ABI (`rt_sigaction`, `sigaltstack`, `siginfo` and `ucontext` frames, restorers), `epoll`/`eventfd`/`timerfd`/`signalfd`, the `getdents64`, `stat`, `statx` layouts, `set_tid_address`, `arch_prctl`, `prlimit64`, `madvise`, a Linux-shaped `/proc/self`, and 32-bit compatibility mode (there is no 32-bit user segment).

## The design

* **A personality per process.** The `syscall` entry looks at a per-process ABI flag: *native* (today's table) or *Linux* (a second table keyed by `rax`, written from the man pages). The flag is chosen at `execve` (a launcher, `linux PROGRAM`, like a user-mode emulator's front end, and later rules by what the file asks for, e.g. an interpreter path in the Linux root) and inherited by `fork`. Native programs are untouched.
* **A Linux root directory** (`/linux` or a volume) holds a user-provided distribution tree; paths a Linux process names are resolved inside it (chroot-like), `/proc` and `/dev` are Linux-shaped views.
* **The loader grows** `PT_INTERP`, `ET_DYN`, `PT_TLS` and a correct initial stack with the auxiliary vector (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`, `AT_RANDOM`, `AT_SECURE`, `AT_HWCAP`, `AT_EXECFN`, `AT_PLATFORM`; a vDSO is optional: glibc and musl fall back to real calls without one).
* **Our own dynamic linker** is a separate, later question for *our* programs (they are static today); Linux programs bring theirs.

## Checkpoints (each has a test that says "done")

| | Target | Test |
|---|---|---|
| **L0** (done 2026-11-03) | a static x86-64 program with no libc | the raw hello of the census runs and prints; `linux PROGRAM` sets the Linux personality (`linux_hello_and_exit_status`, `linux_initial_stack_matches_linux`, `linux_probe_matches_linux`) |
| **L1** (done, 2026-11-03; sockets still to add to the corpus) | static glibc programs | a static glibc hello, a tour of the C library, directories (`opendir`/`readdir`), system information, signals and POSIX threads, each compared line by line with its output on a real Linux (`linux_static_glibc_programs`, `linux_static_glibc_signals`, `linux_static_glibc_threads`) |
| **L2** | dynamically linked programs | `PT_INTERP`/`ET_DYN`/auxv; a musl or glibc `ls`, `cat`, `sh` from a user-provided root |
| **L3** | big programs | Python (threads, sockets, `hashlib`), `gcc -c`, a shell pipeline of coreutils: the census programs themselves run unchanged |
| **L4** | graphics and sound | a minimal Wayland compositor over Fairies (shared-memory buffers only), SDL2's software path; ALSA over `/dev/audio` |
| **L5** | bigger and stranger | `epoll`-heavy servers, `ptrace`-free debuggers, 32-bit programs (compatibility mode, shared with the Windows plan) |

Rough sizing, in "agent sessions" like the ones that wrote the web engine: L0 and L1 are one or two, L2 two, L3 three to five (signals and `clone` are the hard part), L4 two to four. They are guesses until L0 and L1 have been done.

## State: what runs now (L0 and L1, 2026-11-03)

`linux PROGRAM [ARGUMENT...]` runs a **static x86-64 Linux executable**: either a program with no C library at all (the kit in `user/linuxtest`) or one linked statically with glibc. `native PROGRAM` is the way back for a native tool started from inside a Linux program. Files and directories, signals (handlers with `siginfo`, the alternate stack, the floating-point state, `SA_RESTART`), threads (`clone` with `CLONE_THREAD`, futexes, thread-local storage) and timers (`alarm`, `clock_nanosleep`) work too; dynamically linked executables, sockets and the real-time signals are not there yet (see "Next" below).

**How it is built**

* **The personality.** `Process.abi` is `ABI_NATIVE` or `ABI_LINUX`. `syscall_dispatch` (`kernel/sys/syscalls.c`) sends a Linux process's calls to `linux_syscall`; everything else is as before. `fork` copies the personality, `execve` keeps it unless the new native call `setabi` (119, used by `linux` and `native`) asked for the other one for that one exec.
* **The table** (`kernel/linux/syscall.c`, numbers in `numbers.h`). Native errno values, open and mmap flags, signal numbers up to 31 and the argument registers are already Linux's, so a call that means exactly what a native call means is routed to the native handler (`NATIVE(...)`); the rest have handlers of their own (`OWN(...)`): `arch_prctl`, `set_tid_address`, `prlimit64`/`getrlimit`, `uname`, `newfstatat`/`stat`/`fstat` with Linux's 144-byte struct, `readlinkat`, `readv`/`writev`, `poll`, `clock_gettime`, the `*at` calls (`openat`, `mkdirat`, `unlinkat`, `renameat`, `faccessat` ...), `pread64`/`pwrite64`, `clone` (the fork forms), `pipe2`, `dup3`, `time`, `gettimeofday`. A call with no entry answers `ENOSYS` and is named once in the kernel log (tag `Outside World`); `linux.trace` on the kernel command line logs every call with its result.
* **Signals, threads, timers.** `kernel/linux/signal.c` has `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigtimedwait`, `sigaltstack`, `tkill` and `rt_sigreturn`, and builds the handler's frame as x86-64 Linux does (`struct rt_sigframe`: the restorer as return address, a `ucontext` with the registers, a `siginfo` with the address and code of a fault, the FXSAVE state below); the generation, blocking and default actions are the native machinery (`kernel/sys/signal.c`), with the signal-set bit order converted. A fault in a Linux program runs its SIGSEGV handler instead of always killing it. `clone` with `CLONE_THREAD` creates a native thread (`task_create_thread`); `futex` (wait, wake, requeue, the bitset forms, private and clock flags, absolute timeouts) is a wrapper over the native futex queues. `alarm` is a real per-process timer now (it was only a reserved number), checked on the scheduler tick; it serves native and Linux programs.
* **The initial stack** (`kernel/linux/stack.c`): argc, argv, envp and the auxiliary vector as the psABI describes them (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`, `AT_ENTRY`, the user ids, `AT_PLATFORM`, `AT_HWCAP`, `AT_CLKTCK`, `AT_SECURE`, `AT_RANDOM`, `AT_HWCAP2`, `AT_EXECFN`), 16-byte aligned. The ELF loader reports where the program headers ended up.
* **How it is checked.** The facts in the code (call numbers, constants, struct layouts) are compared with a real Linux by `tests/host/test_linux_abi.py`. The behaviour is compared by running the same program on a real Linux (`tools/linux-oracle.py`, in WSL) and on TouhouOS (`tests/case_linux.py`) and diffing the output line by line: the freestanding programs in `user/linuxtest` (a 90-line probe that makes many calls on purpose wrong, the stack and auxv checker, exec) and ordinary C programs in `tests/linux/src` linked statically with glibc, which the test downloads into the guest from a host HTTP server (they are build products in `build/linux-inputs`, never committed, never part of an image). The stored outputs are in `tests/linux/golden`.
* **Adding a call.** Add its number to `numbers.h` (the host test then checks it against the headers of the real system), route it to a native handler or write one, add a line to a probe or a corpus program that exercises it and its errors, regenerate the stored output with `wsl.exe -e python3 tools/linux-oracle.py`, and make the guest print the same. When a program asks for something that is missing, `dmesg | grep "Outside World"` names it.

**Approximations to know about** (each is a place where behaviour differs from Linux on purpose, for now)

* A `clone` with `CLONE_VM` (posix_spawn, `system()`) gives the child a *copy* of the address space, not a shared one: the exec that follows works, but a program that passes data back through shared memory before the exec does not. `clone3` answers `ENOSYS`, which makes the C library fall back to `clone`.
* `prlimit64`/`setrlimit` report this system's limits (an 8 MiB stack, 64 descriptors) and accept new values without enforcing them. `uname` reports a Linux release string (`6.1.0-gensokyo`) so that programs that check the kernel version are satisfied. The CPU-time clocks of `clock_gettime` answer `EINVAL`; the coarse and boot-time clocks are the ordinary ones.
* `exit` ends the calling thread, and the last thread out ends the process. `vfork` is `fork`. `fsync`, `flock` and `madvise` do nothing. The environment and arguments of a program are limited to 32 KiB together.
* Signals: the real-time signals 32 to 64 do not exist (the native masks hold 31 signals), so `pthread_cancel` and the C library's set-id broadcast do not work. A `siginfo` carries the address and code of a fault but not who sent a signal (`si_pid`, `si_uid` are zero and `si_code` is 0 for `kill` and `raise`). `setitimer` and POSIX timers are not there; `alarm` is. Only `read`, `write`, `readv`, `writev`, `pread64`, `pwrite64`, `wait4`, `open` and `openat` are restarted after an `SA_RESTART` handler.

**Next, in this order**: (1) dynamically linked executables (`ET_DYN`, `PT_INTERP`, the interpreter's load address in `AT_BASE`, file-backed `mmap` with the flags and offsets a dynamic linker uses), which also needs (2) a Linux root directory (`/linux`) that the paths of a Linux process are resolved in, with Linux-shaped `/proc/self` and `/dev`; (3) the real-time signals (the native masks grow to 64 signals), `setitimer`, and who-sent-it in `siginfo`; (4) sockets, `epoll`, `eventfd`, `timerfd`; (5) choosing the personality at `execve` by itself (a native ELF note in our own programs, `PT_INTERP` and `.note.ABI-tag` in theirs) so that a Linux program can be started from the shell by name; (6) then the checkpoint after that, L3: the census programs themselves (`ls`, `cat`, `sh`, Python, `gcc -c`) from a user-provided root.

## Open questions

* Is the native ABI ever replaced by Linux's? (No for now: native stays ours, Linux is a translation.) A native `libgap` could later gain a dynamic linker of its own.
* The Linux root: a directory on the `HUMANVILLAGE` volume, a second volume, or an image? It decides where `/lib64/ld-linux-x86-64.so.2` lives and how a user installs a distribution tree.
* How much of `/proc` and `/sys` to fake, and how: decided by what the checkpoints need, not up front.
* Windows: see the next page when it exists. The shared pieces are the 32-bit compatibility mode, the loader work and the graphics path (software rendering; a GPU driver is its own project).
