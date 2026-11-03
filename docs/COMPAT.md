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
| **L0** | a static x86-64 program with no libc | the raw hello of the census runs and prints; `execve` of it sets the Linux personality |
| **L1** | static glibc programs | the 17-call static hello; then our own corpus of small C programs built with `gcc -static` in WSL (files, `getdents`, `mmap`, `fork`/`wait`, pipes, threads and `futex`, signals, sockets), each compared with its output on real Linux |
| **L2** | dynamically linked programs | `PT_INTERP`/`ET_DYN`/auxv; a musl or glibc `ls`, `cat`, `sh` from a user-provided root |
| **L3** | big programs | Python (threads, sockets, `hashlib`), `gcc -c`, a shell pipeline of coreutils: the census programs themselves run unchanged |
| **L4** | graphics and sound | a minimal Wayland compositor over Fairies (shared-memory buffers only), SDL2's software path; ALSA over `/dev/audio` |
| **L5** | bigger and stranger | `epoll`-heavy servers, `ptrace`-free debuggers, 32-bit programs (compatibility mode, shared with the Windows plan) |

Rough sizing, in "agent sessions" like the ones that wrote the web engine: L0 and L1 are one or two, L2 two, L3 three to five (signals and `clone` are the hard part), L4 two to four. They are guesses until L0 and L1 have been done.

## Open questions

* Is the native ABI ever replaced by Linux's? (No for now: native stays ours, Linux is a translation.) A native `libgap` could later gain a dynamic linker of its own.
* The Linux root: a directory on the `HUMANVILLAGE` volume, a second volume, or an image? It decides where `/lib64/ld-linux-x86-64.so.2` lives and how a user installs a distribution tree.
* How much of `/proc` and `/sys` to fake, and how: decided by what the checkpoints need, not up front.
* Windows: see the next page when it exists. The shared pieces are the 32-bit compatibility mode, the loader work and the graphics path (software rendering; a GPU driver is its own project).
