# Windows programs: winrun

The owner's list has "Windows programs" on it; `docs/COMPAT.md` left the question open. This page is the answer's first step. The rule is COMPAT.md's: original code, from
**Microsoft's published specifications and documentation** (the "PE Format" specification, the x64 calling convention, the Win32 API reference pages on Microsoft Learn) and
from our own experiments with the LLVM tools on the development PC. No Wine, no ReactOS, no other implementation's code, no file from a Windows installation (no DLL, no SDK
header: the programs' and the layer's declarations are our own). Every file names the specification it follows in its header comment.

## The design: a loader in user space

`winrun PROGRAM.EXE [arguments]` is an ordinary native program (`user/bin/winrun/`). It

1. **reads and checks** the file (`pe.c`): the DOS header and `e_lfanew`, the `PE\0\0` signature, the COFF header (machine 0x8664, an executable image, not a DLL), the PE32+
   optional header (magic 0x20B, alignments, `SizeOfImage`, `SizeOfHeaders`, the console subsystem, the data directories), every section header. It trusts nothing: every
   offset and size is checked against the file's length, `SizeOfImage` and the other headers (sections ascending, page-aligned, not overlapping each other or the headers,
   raw data inside the file and not over the headers, `SizeOfImage` exactly the end of the last section, the entry point inside an executable section, every data directory
   inside the sections). A file that is not exactly valid is refused with a one-line reason and **exit status 126**.
2. **maps** it (`load.c`): an anonymous mapping of `SizeOfImage` bytes at `ImageBase` (`mmap` with `MAP_FIXED`) when that address is safe (see "What the kernel allowed"),
   otherwise wherever the kernel puts it, then the headers and the sections are copied in; the part of a section beyond its raw data (`VirtualSize` greater than
   `SizeOfRawData`) stays zero. A moved image gets its **base relocations** (`IMAGE_REL_BASED_DIR64`, `ABSOLUTE` skipped; any other type is refused); the table is checked
   completely before the first fix-up is written. An image with no relocations that cannot sit at its base is refused.
3. **resolves the imports** (`imports.c`): the import directory, the lookup tables (names and ordinals, reserved bits checked, every string terminated inside the image),
   and the import address table, which gets the address of the built-in function. **A DLL that is not built in is an error at load time** (126). **A function that is not
   built in is an error only when it is called**: it gets a stub of its own, which prints `winrun: unimplemented KERNEL32.dll!Name` on standard error and ends the process with
   **status 127**. Programs import more than any one run uses; this keeps them going as far as the layer reaches.
4. **sets the protections** the sections ask for (`mprotect`: read, write, execute from the section characteristics; the headers read-only), after the import address
   table is written.
5. **builds the command line** the way a Windows program expects to parse it (arguments separated by a space; one with a space, a tab or a quote is quoted, backslashes
   doubled in front of a quote) and **calls the entry point** with the Windows x64 convention: the entry is an `ms_abi` function pointer, so the compiler converts the
   registers, the 32-byte shadow space and the stack alignment. `ExitProcess(code)` ends the native process with that code; a return from the entry ends it with the returned
   value. The shell's `$?` shows it.

The **Win32 layer** (`win32.c`) is compiled into winrun: every function is `__attribute__((ms_abi))` and calls the native libc. The built-in DLL table has one entry today,
`KERNEL32.dll` (compared without case); `ntdll` and a C runtime come later.

## State (W0, W1 and the first part of W2)

Implemented, from the documented behaviour (return values and the last-error codes the documentation names on failure):

* **W0:** `ExitProcess`, `GetStdHandle` (-10, -11, -12 as handles 4, 8 and 12 of winrun's handle table), `WriteFile`, `WriteConsoleA` (a terminal only), `ReadFile`
  (end of file is TRUE with 0 bytes), `GetLastError`, `SetLastError`.
* **W1:** `GetCommandLineA`, `GetCommandLineW` (UTF-8 decoded to UTF-16), `GetModuleHandleA/W(NULL)` (the image base; a name is `ERROR_MOD_NOT_FOUND`), `GetProcessHeap`,
  `HeapAlloc` (with `HEAP_ZERO_MEMORY`), `HeapFree`, `HeapReAlloc` (with `HEAP_REALLOC_IN_PLACE_ONLY` and `HEAP_ZERO_MEMORY`), `HeapSize`, `VirtualAlloc` (reserve and commit
  together, the page protections mapped to `PROT_*`; a commit inside a region already handed out), `VirtualFree` (`MEM_RELEASE` with size 0, `MEM_DECOMMIT` zeroes),
  `GetEnvironmentVariableA` (the documented return values: length without the NUL, or the size needed, or 0 and `ERROR_ENVVAR_NOT_FOUND`), `lstrlenA`, `GetTickCount`,
  `GetTickCount64`, `QueryPerformanceCounter`/`QueryPerformanceFrequency` (10 MHz), `GetCurrentProcessId`, `Sleep`, `GetFileType`, `GetConsoleMode`/`SetConsoleMode`
  (a terminal answers; the mode is not acted on).
* **Files:** `CreateFileA` (the five creation dispositions, `ERROR_ALREADY_EXISTS` for `CREATE_ALWAYS`/`OPEN_ALWAYS` on an existing file), `CloseHandle`, `GetFileSize`,
  and `ReadFile`/`WriteFile` on those handles, over a 64-entry handle table on the native `open`/`read`/`write` (the `files` test program).
* **W2:** `TlsAlloc`, `TlsFree`, `TlsGetValue` (clears the last error on success, as documented), `TlsSetValue` (the 64 slots are the TEB's `TlsSlots`),
  `GetCurrentThreadId`, `GetCurrentProcess`/`GetCurrentThread` (the pseudo-handles -1 and -2), `IsDebuggerPresent`, `InitializeCriticalSection(AndSpinCount)`,
  `EnterCriticalSection`, `TryEnterCriticalSection`, `LeaveCriticalSection`, `DeleteCriticalSection` (one thread: counting in `LockCount`, `RecursionCount`,
  `OwningThread`), `GetModuleFileNameA` (the path as given to winrun), `GetSystemTimeAsFileTime`, `SetConsoleCtrlHandler` (accepted, not acted on),
  `GetConsoleCP`/`GetConsoleOutputCP` (65001), `SetConsoleOutputCP` (65001 only), `FlushFileBuffers`; `SetFilePointer`, `SetFilePointerEx` (`ERROR_NEGATIVE_SEEK` before the start), `GetFileAttributesA` (directory, or archive as Windows reports a new file; read-only without a write bit), `DeleteFileA`, `CreateDirectoryA` (`ERROR_ALREADY_EXISTS`), `RemoveDirectoryA`, `GetCurrentDirectoryA`, `MultiByteToWideChar`/`WideCharToMultiByte` (CP_UTF8, and CP_ACP as UTF-8; surrogate pairs; `ERROR_INSUFFICIENT_BUFFER`).

Not there: `HeapCreate` and private heaps, `CommandLineToArgvW`, wide-character file names (`CreateFileW`), `FindFirstFile`, wide-character names in general,
`GetStartupInfo`, `GetEnvironmentStrings`, `WriteConsoleW`, overlapped I/O, threads (`CreateThread`), anything else.


## W2: the GS base, the TEB and the PEB

**The kernel part** (2026-11-24, the only kernel change of the Windows work): `SYS_gsbase` (number 127, `shared/syscall.h`), in libc `gap_set_gsbase(base)` and
`gap_get_gsbase()`. It sets the calling thread's `gs:` base: `Task.gs_base`, written to `IA32_GS_BASE` with `wrmsr` (not `WRGSBASE`: `CR4.FSGSBASE` stays off, so it
works on the Kaby Lake Dell and under TCG alike) and loaded at every task switch next to the FS base (`kernel/sched/task.c`). The kernel itself never uses `gs` (no
`swapgs`, no per-CPU area behind it; `gdt.c` loads a zero-based selector at boot), so the user value can simply stay loaded while the kernel runs. The rules: **fork
inherits** the base, **a new thread inherits** its creator's (both through `ThreadInit`), **execve clears** it, a **signal handler** runs with the thread's base and leaves it
alone (signals do not touch it at all). A base at or above `0x800000000000` is `-EINVAL`. A Linux-personality clone does not inherit it (Linux programs use
`arch_prctl`, which still has no `ARCH_SET_GS`). Test: `user/test/gstest.c` (`windows_gs_base_per_thread`): two threads with different bases read `gs:[0]` 3000 times
each across yields and sleeps, a signal handler, a new thread, a forked child, an exec'd copy.

**The TEB and the PEB** live in winrun's own data (one thread: one TEB of two pages, one PEB page, the process parameters), and `gs` points at the TEB before the entry
call. The offsets are the x64 layout of the structures Microsoft documents in `winternl.h` (TEB: `NT_TIB` first, `ProcessEnvironmentBlock` after twelve pointers,
`TlsSlots`; PEB: `BeingDebugged`, `ProcessParameters`; `RTL_USER_PROCESS_PARAMETERS`: `ImagePathName`, `CommandLine`) and the ones a C runtime is known to read; the
`teb` test program checks every one of them **on this PC's real Windows 11 first** (all 15 checks pass there) and then under winrun:

| offset | field | winrun puts there |
|---|---|---|
| TEB `0x08` / `0x10` | StackBase / StackLimit | the top and the bottom of the program's own 1 MiB stack (winrun runs the entry on it) |
| TEB `0x30` | Self | the TEB's address (what `gs:[0x30]` reads) |
| TEB `0x40` / `0x48` | ClientId: process id / thread id | `getpid()` / `gettid()` |
| TEB `0x58` | ThreadLocalStoragePointer | an array of one null pointer (images with a `.tls` directory are still refused) |
| TEB `0x60` | ProcessEnvironmentBlock | the PEB |
| TEB `0x68` | LastErrorValue | **the** last error: `GetLastError`/`SetLastError` use it, so an inlined `gs:[0x68]` read agrees |
| TEB `0x1480` | TlsSlots[64] | `TlsGetValue`/`TlsSetValue` |
| PEB `0x02` | BeingDebugged | 0 |
| PEB `0x10` | ImageBaseAddress | the image base |
| PEB `0x20` | ProcessParameters | `ImagePathName` at `+0x60`, `CommandLine` at `+0x70` (`UNICODE_STRING`: lengths in bytes, buffer at `+8`; the same buffer `GetCommandLineW` returns) |

Everything else is zero. Without `SYS_gsbase` (an older kernel) winrun still runs programs that do not use `gs`, and `-v` says the base was not set.

## The mini C runtime (`tests/win/crt`)

Our own, from the C standard and the Win32 documentation: `mainCRTStartup` (argv from `GetCommandLineA` with Microsoft's documented rules: `argv[0]` up to a space or
in quotes, then backslashes are literal except in front of a quote, `2n` backslashes and a quote are `n` and a delimiter, `2n+1` are `n` and a literal quote, `""` inside
quotes is a quote), `exit` and 32 `atexit` handlers (in reverse order), `abort` (status 3), `malloc`/`free`/`calloc`/`realloc` over `HeapAlloc`, `qsort`, `atoi`, the
string functions (`memcpy memmove memset memcmp strlen strcmp strncmp strchr strcpy strcat`), `vsnprintf`/`snprintf`/`printf`/`fprintf`/`puts`/`putchar`/`fputs`/`fflush`
(`%d %i %u %x %X %o %c %s %p %%`, `l ll z h hh`, width and `*`, precision, `- 0 + space #`; stdout buffered and flushed at a newline and at exit, stderr not),
`__chkstk` (the compiler calls it with the frame size in `rax` before a frame of 4 KiB or more; it touches every page from the top down and changes only `r10`, `r11`)
and `_fltused`. Programs: `fizzbuzz` (also printf's formats), `sort` (20000 numbers), `strings` (and the argv quoting, `atexit`), `deep` (5000-deep recursion and a
64 KiB frame through `__chkstk`, inside the TEB's stack bounds), `checksum FILE` (FNV-1a over `ReadFile`), `teb` (the table above), `fsops DIR` (directories, attributes, file positions, the UTF-8 conversions). Their expected output was taken from
running the same `.exe` files on Windows 11 on this PC (a 20000-deep recursion overflowed Windows' 1 MiB stack there, so `deep` uses 5000, and winrun's stack is also 1 MiB).

## Building and running the examples

```
python tools/make-winprogs.py          # tests/win/src/*.c and tests/win/crtsrc/*.c -> build/winprogs/*.exe (never committed)
python tools/hosttest.py discover -s tests/host -p "test_winrun.py" -v
python tools/qemu-slot.py -- python tests/run_all.py windows
```

The test programs are freestanding C (`tests/win/src/win.h` declares the calls; no C runtime): `clang --target=x86_64-pc-windows-msvc -ffreestanding -nostdinc
-fno-builtin -fno-stack-protector -mno-stack-arg-probe -O1 -c`, then `lld-link /nodefaultlib /entry:start /subsystem:console /base:... /fixed:no` against import libraries
that `llvm-dlltool -m i386:x86-64` makes from `tests/win/kernel32.def` and `user32.def`. In the guest: copy an `.exe` in (the test downloads them with `fetch` from the host)
and run `winrun hello.exe`; `winrun -v` prints where each section went, whether the image was moved, and how every import was resolved.

| program | shows |
|---|---|
| `hello` | `WriteFile` to standard output, `ExitProcess(0)` |
| `exitcode` | `ExitProcess(42)` is `$?` 42 |
| `args` | the command line as Windows builds it, the wide copy, `GetModuleHandleA(NULL)`, an environment variable, error 203, a return from the entry (3) |
| `heap` | 4096 heap operations with patterns, `HeapSize`, `HeapReAlloc`, a 1 MiB `VirtualAlloc`, the clocks |
| `unused_import` / `called_import` | an import winrun lacks: harmless until called, then the message and 127 |
| `reloc` / `reloc_low` | absolute addresses in data; the second is linked at 0x400000, where winrun itself lives, so it is moved and relocated |
| `otherdll` | an import from `USER32.dll`: refused at load time, 126 |
| `files` | `CreateFileA` and its dispositions and last errors, `WriteFile`, `GetFileSize`, `ReadFile` to the end, `CloseHandle` twice, a missing file |

The host test builds the parser with the address and undefined-behaviour sanitizers, compares its view of every test program with `llvm-readobj` (headers, sections,
imports, relocations), runs 9000 damaged copies (truncated, bits flipped, fields overwritten; fixed seeds) without a crash, and refuses fourteen hand-made damaged files
each with its own reason.

## What the kernel allowed

* **Fixed placement at 0x140000000 works** with the native `mmap(MAP_FIXED)`; the kernel's user range goes to `0x800000000000`. But `MAP_FIXED` *replaces* whatever is
  mapped there (`do_mmap` unmaps first), so winrun asks for a fixed address only where nothing of its own can be: above its own program and the 1 GiB its `brk` heap may
  grow into (plus 64 MiB), and below the kernel's mmap area (`USER_MMAP_BASE`, `0x100000000000`). Any other base is relocated. A `MAP_FIXED_NOREPLACE` (fail instead of
  replace) in the kernel would make this exact instead of conservative.
* **`mprotect`** on the image works; the sections get their own protections. (If it ever failed, winrun says so with `-v` and the section stays read-write.)

## Out of scope (for now)

Graphics (`user32`, `gdi32`), COM, the registry, structured exception handling (`.pdata` unwinding, vectored handlers), DLLs that come with the program
(`LoadLibrary`), 32-bit images (compatibility mode; shared with the Linux L5 plan), services.

## Next steps

* **The rest of W2:** threads (`CreateThread` over the native threads, each with its own TEB and `gs` base: the kernel part is ready, new threads inherit the base and set
  their own), static TLS (`.tls` directory, `ThreadLocalStoragePointer` per thread, TLS callbacks), `HeapCreate`, wide file names (`CreateFileW`), `FindFirstFile`,
  `GetStartupInfo`, `GetEnvironmentStringsW`, `WriteConsoleW`.
* **W3:** structured exception handling (unwinding through `.pdata`/`.xdata` from the x64 exception specification), a first real program, and a C runtime good enough
  for a program built by someone else.

## Starting a Windows program by its name (W3's first step, done)

`execve` of a file whose first two bytes are `MZ` (and that is at least 64 bytes long) is handled like a script that begins with `#!/bin/winrun`
(`kernel/sys/exec.c`, the interpreter rule): the kernel runs `/bin/winrun PATH args...`, so `chmod +x hello.exe; ./hello.exe` works from the shell, `$?` is the program's
status, and `argv[0]` of the Windows program is the path as typed. The file needs the execute bit like any program. The `#!` depth limit counts it too.
