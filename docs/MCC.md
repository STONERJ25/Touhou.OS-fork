# mcc: Marisa's compiler (with mld and mar)

TouhouOS can compile programs for itself. `mcc` is a C compiler that runs on the system and produces static x86-64
TouhouOS executables, `mld` is the linker it calls, `mar` the archiver. The source (`user/bin/mcc/`) is plain C that
`mcc` itself accepts, so the system can rebuild its own compiler, and the result is byte for byte the program that built it.

| Command | Where | What |
|---------|-------|------|
| `mcc`, `cc` | `/bin/mcc`, `/bin/cc` | driver: preprocess, compile, link (`-c`, `-E`, `-o`, `-I`, `-D`, `-U`, `-l`, `-L`, `-s`, `-M`...) |
| `mld` | `/bin/mld` | linker: objects and archives to an executable with the layout of `user/user.ld` |
| `mar` | `/bin/mar` | archiver: `mar rcs libx.a a.o b.o`, `t`, `x`, `d` |
| manuals | `mcc(1)` (also `cc`), `mld(1)`, `mar(1)` | `man mcc` |

All three are one program (`mcc`) that looks at `argv[0]`; `/bin/cc`, `/bin/mld` and `/bin/mar` are links (`EXTRA_LINKS` in
`tools/build.py`). On a host without links: `mcc --mld ...` and `mcc --mar ...`.

## Using it

```
reimu@gensokyo ~ $ cat hello.c
#include <stdio.h>
int main(void) { puts("Hello from Gensokyo!"); return 0; }
reimu@gensokyo ~ $ mcc hello.c -o hello && ./hello
Hello from Gensokyo!
reimu@gensokyo ~ $ cd /ScarletDevilMansion/src/mcc && mcc main.c -o /tmp/mcc2 && /tmp/mcc2 --version
mcc 0.1 -- Marisa Kirisame's compiler, a C11 subset for x86-64 TouhouOS (ze!)
```

A compile error is `file:line: error: text`, the source line, a caret and a remark in Marisa's voice (the plain message
stays first, see THEMING.md); compilation stops at the first error and no object is written.

## The pipeline

```
 file.c --tokenize--> tokens --preprocess--> tokens --parse--> AST (typed) --codegen--> machine code + relocations
                                                                                          |
                          crt0.o, libgap.a, libkappa.a ... --mld--+-- ELF .o in memory --+--> static ELF executable
```

Everything is one translation unit: `user/bin/mcc/main.c` includes the `.inc` files in this order.

| File | Role |
|------|------|
| `mcc.h` | shared types: `Token`, `Type`, `Node`, `Obj`, ELF structures |
| `util.inc` | allocation, byte buffers, hash map, file reading |
| `type.inc` | the type objects, `add_type` (types of expressions, usual arithmetic conversions) |
| `lex.inc` | diagnostics, the tokenizer, number and string literals, `mcc_strtod` (the same result in every build of mcc) |
| `pp.inc` | the preprocessor: Prosser's hide sets, `#`/`##`, `__VA_ARGS__` and `__VA_OPT__`, `#include`/`#include_next`/`#pragma once`, `#if` in 64-bit arithmetic (the unevaluated side of `&&`, `||`, `?:` may divide by zero, `__has_include`), predefined macros, `-E` output |
| `parse.inc`, `expr.inc` | declarations, struct layout, initializers, constant folding, expressions, statements, builtins, atomics |
| `x86.inc` | output sections, relocations and the instruction encoder (`mov`, `lea`, ALU, SSE, jumps) |
| `asm.inc` | inline assembly: constraint handling and the small AT&T assembler |
| `gen.inc` | the code generator: AST to machine code, the System V calling convention, `va_arg`, atomics |
| `obj.inc` | data emission and the ELF64 relocatable object writer |
| `ld.inc` | `mld` |
| `ar.inc` | `mar` |
| `main.c` | the drivers `mcc`, `mld`, `mar` |
| `include/` | the freestanding headers that belong to the compiler (`stddef.h`, `stdarg.h`, `stdint.h`, `limits.h`, `float.h`, `stdbool.h`...) |

### Code generation

A plain accumulator machine, as in chibicc (on which the parser design is modelled, written from scratch for this project): an
expression leaves its value in `rax` (integers, pointers, the address of a struct) or `xmm0` (`float`, `double`);
binary operators evaluate the right operand, push it, evaluate the left one and pop the right one into `rdi`. Cheap peepholes
avoid the push/pop when the right operand is a constant or a local variable, loads and stores of variables with a known `rbp`
or `rip` address use that address directly, comparisons feed `jcc` without materialising 0/1, a `jmp` to the label that follows
it is dropped again, and the stack slots of locals in sibling blocks (and the temporaries of finished statements) are shared,
which keeps recursive programs within the stack. Integer operations and casts whose operands are constants are folded when the
tree is typed (`add_type` and `new_cast` in `type.inc`, with the wrap-around of the target type), so `(char)300`, `1 << 12` or
`sizeof(x) * 4` cost no instructions. `long double` is `double`.

The compiler's own recursion (the parser, `add_type` and the code generator walk the tree recursively) is deep for generated
sources: the driver runs the whole compile on a 256 MiB pthread stack and falls back to the main thread (8 MiB) if the thread
cannot be made. A 4,000-term expression chain compiles.

Calls follow the System V AMD64 convention, so objects made by clang and by mcc mix: integer and SSE registers, structs up to
16 bytes classified per eightbyte, bigger ones in memory (and returned through a hidden pointer), `al` = number of vector registers
for variadic calls, `va_list` with the real 24-byte layout and 176-byte register save area. Thread-local variables use the
local-exec model (`mov rax, fs:0; lea rax, [rax + x@tpoff]`, `R_X86_64_TPOFF32`), which is what `user/libc/src/pthread.c` expects.

Unused `static` functions and variables, and `static inline` functions nobody calls, are not emitted. Strings and `const`
data go to `.rodata`; uninitialised data to `.bss` (tentative definitions are not COMMON). Identical string literals of a
translation unit are one object (`"" == ""`), as with gcc and clang; some programs depend on it.

`memcpy`, `memmove` and `memset` (and so `__builtin_memcpy` and friends) with a constant size are expanded in line when the
callee is the library function: `memcpy` and `memset` up to 64 bytes, `memmove` up to 32 (all loads before all stores), `memset`
only with a constant fill byte. libgap's versions are `rep movsb`, which costs hundreds of cycles to start on an emulated
machine, and portable code writes unaligned loads as `memcpy(&v, p, 8)`.

### Objects and the linker

`mcc` writes ordinary ELF64 `ET_REL` objects (`.text .data .rodata .bss .tdata .tbss`, `.rela.*`, `.symtab`, `.note.GNU-stack`)
with `R_X86_64_PC32`, `PLT32`, `64` and `TPOFF32` relocations. The driver keeps the objects in memory and hands them to the
linker; with `-c` it writes them.

`mld` reads relocatable objects and GNU/BSD `ar` archives (those of `llvm-ar` included, `libgap.a` is one) and lays the program
out like `user/user.ld`: text at 0x400000 (file offset 0x1000), then rodata, then data/tdata/bss each on its own page, a
PT_TLS segment, entry `_start`. Archive members are pulled in until no undefined symbol can be satisfied (the order of
libraries does not matter), weak and COMMON symbols are handled, `GOTPCREL(X)` and `GOTTPOFF` get a small GOT in the data
segment, `TPOFF32` uses the formula of libgap's thread start-up. It defines `__tls_*` for libgap. The output has a section table
and a symbol table (unless `-s`), so `llvm-objdump -d` works on the programs it makes.

## Language support

C11 without variable-length arrays and complex numbers. Everything of a normal program works: all declarators, structs, unions,
bit-fields, anonymous members, flexible array members, designated and nested initializers (also of automatic variables),
compound literals, statement expressions, case ranges, `_Generic`, `_Static_assert`, `typeof`, `_Alignas/_Alignof`, `_Bool`,
`long long`, `float`/`double`, `inline`, `restrict`, `_Thread_local`, `__func__`, `goto`, string and character literals including
wide ones, K&R-style calls to undeclared functions. The builtins are listed in `mcc(1)`: `__builtin_va_*`, `offsetof`,
`bswap`, `clz/ctz/popcount`, `__atomic_*`, `__sync_*`...

Not supported (a clear error, or documented ignoring): variable-length arrays, `_Complex`, computed `goto`, `alloca`, K&R
function definitions, x87/SSE inline assembly, `-S` (there is no assembly text; use `llvm-objdump -d`), shared objects. `__attribute__` is parsed; `packed`, `aligned`, `weak` are honoured. Warnings do not exist.

## Tests

`python tools/build.py` first (the tests use `build/initrd.img` and the staged system root `build/mcc-root`), then
`python tests/run_all.py mcc` and `... mld`:

| Test | Where | What it proves |
|------|-------|----------------|
| `mcc_preprocessor` | host | `mcc -E` produces the token stream of `clang -E` for a macro torture file (`tests/mcc/pp1.c`) and every libgap header |
| `mcc_diagnostics` | host | 16 mistakes give `file:line: error: ...`, a caret, Marisa's remark, exit status 1 and no object |
| `mcc_object_files` | host | objects are valid ELF (`llvm-readobj`, `llvm-objdump`), `mar` archives are read by `llvm-ar` and `llvm-nm` |
| `mcc_corpus` | QEMU | the 16 programs of `tests/mcc` (arithmetic, pointers, structs, floats, varargs, control flow, statics and TLS, calls, declarators, preprocessor, algorithms, data structures, SHA-256/CRC32, atomics, inline assembly, memcpy/memmove/memset and string literals) compiled on the host print the expected text |
| `mcc_on_guest` | QEMU | `mcc` running on TouhouOS compiles and links `tests/mcc/guest/*.c` (stdio, libm, pthreads with TLS and atomics, loopback sockets, fork/pipe/exec/signals, inline assembly), error handling, `-c` followed by `mld` |
| `mcc_selfhost` | QEMU | stage 2 (the compiler built on the host by the gcc-built stage 1), the guest's stage 2 (built by the clang-built `/bin/mcc`) and stage 3 (built by stage 2) are the same file; stage 3 compiles programs correctly and writes the same objects and executables as the clang-built compiler |
| `mcc_kappabox` | QEMU | all of kappabox (about 130 applets, 15,000 lines) compiled by mcc passes the 95-line shell test table |
| `mld_relinks_clang` | QEMU | programs relinked by `mld` from clang's objects run like the originals |
| `mld_got_relocations` | QEMU | an object made by `clang -fPIC` (so full of `GOTPCREL` relocations) is linked statically by `mld` and runs |
| `mcc_libgap` | QEMU | libgap itself (all of it but `math.c` and `fpconv.c`, which use x87 `long double` and `unsigned __int128`, and `setjmp.asm`), compiled by mcc, links with `utest` compiled by mcc and passes the OS's own user-space suite (libc, fork/exec, signals, mmap, 22 thread sections) |
| `mcc_userland` | QEMU | libgap, the five libraries and 34 programs of `user/bin` and `user/sbin` (init, login, gapsh, the greeter, the compositor, the panel...) all built by mcc replace the clang builds in the initrd: the desktop comes up, `utest` passes, the shell test table passes |
| `mcc_driver_options` | host | `-D -U -I -include -E -o -c -s -M`, error exits, undefined and duplicate symbols |
| `mcc_fuzz` | host | 40 random programs (integer expressions over every type, compound assignments, floating point) print the same numbers when built by mcc and by gcc |

The expected output of the programs is the output of the same sources built by gcc against glibc (the corpus is portable C;
`tools/mcc_lintest.sh` does that comparison in WSL), so `.out` files are independent of mcc.

Beyond these test cases, the whole userland built by mcc (everything but the calculator, whose engine uses x87 assembly, and
libgap's `math.c` and `setjmp`) was run against the repository's other suites: put `build/mcc-user/{bin,sbin}/*` into a private
copy of the initrd (`U.make_esp` in `tests/mcc_util.py`) and point `tests/run_all.py` at it. That run (123 tests: apps, audio,
awk, bc, dc, diff, patch, games, image, kshell, vi, pnote, pager, shrine, notebook, sketchbook, selftests, crash, blk, usb, gui,
danmaku, sound) gave 111 PASS and 12 FAIL; all 12 are in the two newest GUI editors (8 `notebook_*`, 4 `sketchbook_*`). For
notebook two causes were found and fixed afterwards (see "State when paused"): `notebook --selftest` compares `""` with `""` and
needs merged string literals, and its `nb_memmove` is written with `__builtin_memcpy(&x, p, 8)`, which was a `rep movsb` library
call per 8 bytes (the editor's benchmark ran 25 times slower than the clang build). The sketchbook failures were not looked at.

### The host build

The Windows clang on the development machine has no C library, so the host copy of mcc is built with the Linux gcc of WSL; on
Linux the same scripts run directly with the host's gcc (`linux_cmd` in `tests/mcc_util.py`, LLVM tools found as `tools/build.py`
finds them, through `TOUHOU_LLVM_DIR` or the `PATH`): `bash tools/mcc_host.sh` produces `build/host/{mcc,mld,mar}` (tests/mcc_util.py runs it). `MCC_ASAN=1 bash tools/mcc_host.sh`
builds `mcc-asan` with the address sanitizer. `tools/mcc_lintest.sh FILE.c...` compiles with mcc and gcc, links both against
glibc and compares what they print; `tools/mcc_compile_all.sh` compiles every C file of the OS tree and lists what mcc cannot
digest yet. The programs mcc makes are TouhouOS ELF files: they only run in QEMU (except the differential tests, which link the
objects against glibc).

| Tool | What |
|------|------|
| `tools/mcc_host.sh` | builds `build/host/{mcc,mld,mar}` with the gcc of WSL |
| `tools/mcc_lintest.sh` | one or more files: mcc and gcc, same output? (the differential test the `.out` files come from) |
| `tools/mcc_fuzz.py` | `--count N --seed S`: random integer and floating-point programs, mcc against gcc (a small Csmith without undefined behaviour) |
| `tools/mcc_stress.sh` | the same comparison, with timing, for whatever generated programs you put in `build/stress/` (many locals, many functions, big tables, odd struct shapes) |
| `tools/mcc_compile_all.sh` | compiles every C file of the OS tree, reports what fails |
| `tools/mcc_build_libgap.sh`, `tools/mcc_build_userland.sh` | libgap, or the whole userland, built by mcc into `build/mcc-libgap`, `build/mcc-user` (used by `mcc_libgap`, `mcc_userland`) |
| `tools/mcc_userland_suites.py` | runs suites of `tests/run_all.py` against an initrd whose programs are the mcc builds of `build/mcc-user` (`--none`: the clang baseline) |
| `tools/mccroot.py` | stages the system root (`include/`, `lib/`, `src/mcc/`) for the initrd and for the host tests |

## The system root

`tools/mccroot.py` stages the tree the compiler expects under `$MCC_ROOT` (default `/ScarletDevilMansion`), copies it into the
initrd, and the host tests use `build/mcc-root`:

```
include/        libgap headers, shared/*.h, the headers of libkappa, libfairy, libz, libimg, libaudio
lib/mcc/include/  the compiler's own headers (stddef.h, stdarg.h, limits.h ...); libgap's limits.h uses #include_next to reach it
lib/            crt0.o  libgap.a  libkappa.a  libfairy.a  libaudio.a  libimg.a  libz.a
src/mcc/        the source of the compiler
```

`user/libc/include/errno.h` has one change for mcc: under `#ifdef __MCC__` it reads the thread pointer with
`__builtin_thread_pointer()` instead of inline assembly.

## Limits

- Inline assembly: GNU extended asm works (`asm.inc`): operands in registers, memory or immediates, the `a b c d S D` constraints,
  matching operands, named operands, register variables, numeric local labels, and a small AT&T assembler for the integer
  instructions programs use (see `mcc(1)`). x87 and SSE instructions are not understood, so the calculator's engine still needs
  clang, and the compositor's non-temporal framebuffer copy (`copy_to_fb` in `user/bin/fairyd/comp.c`) falls back to plain
  stores under `__MCC__`. File-scope `asm` is ignored with a warning.
- `unsigned __int128` does not exist, so libgap's `math.c` and `fpconv.c` (exact `strtod`/`printf`) still need clang; the rest of
  libgap is built by mcc in the `mcc_libgap` test. SSE2 intrinsics: `include/emmintrin.h` has the seven that libgap's
  `string.c` uses, in plain C on two 64-bit halves (same results, no speed); anything else is a compile error.
- Code quality: no register allocation, no constant propagation across statements; programs run perhaps 3 to 5 times slower than
  clang -O2 output, and code that leans on the optimiser (a byte loop clang turns into `memcpy`, tiny helpers it inlines) can be
  far slower: the notebook editor's benchmark was 25 times slower until `memcpy` of a constant size was expanded in line. The
  obvious next steps are keeping locals in callee-saved registers and a peephole pass over the byte buffer.
- Diagnostics stop at the first error; there are no warnings. The parser recovers from nothing. `#pragma pack` is ignored.
- `long double` is `double`; there is no x87 code.
- A Windows-native build of the host tools would need a small C runtime shim (no hosted headers are installed); WSL is used instead.
- To add a language feature: parser in `parse.inc`/`expr.inc`, type rules in `type.inc` (`add_type`), code in `gen.inc`, then a program
  in `tests/mcc/` (check it with `tools/mcc_lintest.sh`, then save gcc's output as `NAME.out`).

## State when paused

The work was paused on request; everything is committed on the branch `worktree-agent-a93e3061dde450b33` of the fork and
`python tools/build.py` builds the head. The six rungs of the original plan:

| Rung | State |
|------|-------|
| 1. preprocessor for libgap and the shared headers | done: the same tokens as `clang -E` for the torture file, every libgap header and a user program |
| 2. C11 front end and type checker | done, but for variable-length arrays and `_Complex` (clear errors); 16 diagnostics are tested |
| 3. x86-64 code generator, ELF64 objects | done: System V calls with structs and varargs, TLS, atomics, inline assembly. `-S` is refused on purpose: machine code is emitted directly, there is no assembly text |
| 4. linker `mld`, archiver `mar` | done: `user/user.ld` layout, archives to a fixpoint, weak and COMMON, GOT, TLS, relinks clang's objects |
| 5. tests and self-hosting | written and passing at the last QEMU run (corpus, mcc on the guest, stage 2 = stage 3 byte for byte, kappabox, libgap, whole userland); see below for what has run since the newest edits |
| 6. integration | `/bin/mcc` with `cc`, `mld`, `mar` links, three manual pages, this file, `STATUS.md`, `THEMING.md`, Marisa's diagnostics |

What has been run, and what has not:

| Run | Result |
|-----|--------|
| `python tools/build.py` at the head | builds |
| host tests `mcc_preprocessor mcc_diagnostics mcc_object_files mcc_driver_options mcc_fuzz` (just before the last commit) | 5 of 5 pass |
| `tools/mcc_lintest.sh tests/mcc/*.c` with the final compiler (native differential run against gcc) | all 16 programs identical, `mem.c` included |
| QEMU: `mcc_corpus mcc_on_guest mcc_selfhost mcc_kappabox mld_relinks_clang mcc_libgap` and `manual` | passed when last run (commit `7b111af`), before the edits listed next |
| QEMU: the userland built by mcc against 123 tests of the other suites (see "Tests") | 111 pass, 12 fail (8 notebook, 4 sketchbook); the binaries predate the final versions of the folding and the peephole |
| QEMU: everything below | **not run** |

Edits that have never run in the guest, newest first: inline `memcpy`/`memmove`/`memset` and merged string literals (the last
commit, written to fix the notebook findings above); constant folding, the `jmp` peephole, the 256 MiB driver thread, the `#if`
short-circuit, and the two new tests `mld_got_relocations` and `mcc_userland` (the commit before). All of them were checked on the
host (native differential runs and the fuzzer against gcc), none in QEMU. Known open items: the notebook and sketchbook suites
with the mcc-built userland (the notebook causes are fixed but not re-run, the sketchbook failures have not been looked at); the
guest has not yet been seen to allocate the 256 MiB thread stack (if it cannot, the driver falls back to the main thread, which
is only a problem for generated sources that nest thousands of levels deep).

### After the merge into `cloud/staging` (2026-10-24, Linux, QEMU 8.2, TCG)

The branch was squash-merged on top of the other parked branches. What they changed under mcc's feet, and the fixes: the libc
branch's `fpconv.c` uses `unsigned __int128` (taken from clang like `math.c`) and its `string.c` uses SSE2 intrinsics (the new
`include/emmintrin.h`, checked against the real intrinsics with gcc on 200,000 random cases); kappabox now needs `-ltls` (libtls
stays a clang build: AES-NI intrinsics); `fsck` includes `shared/fatck.c` (`-I shared` in `mcc_build_userland.sh`, as in
`tools/build.py`); the compositor's SSE copy (see "Limits"). `mcc_selfhost` compared `$n.a` with a file that is never written
(`$n.3` for `${n}3`); fixed. The test helpers run natively on Linux now (see "The host build").

`python tests/run_all.py mcc,mld,manual` (with `TOUHOU_TIMEOUT_SCALE=3`): all 13 `mcc_*`/`mld_*` tests and `manual` pass; four
(`mcc_selfhost`, `mcc_kappabox`, `mcc_libgap`, `mcc_userland`) failed for the reasons above and passed after the fixes
(`mcc_userland` included: the mcc-built userland boots the desktop and passes `utest` and the shell table). Not run: the other suites against the mcc-built
userland (`tools/mcc_userland_suites.py`, item 4 below), so the notebook and sketchbook findings are still open.

## How to resume

1. `python tools/build.py`, then the host tests: `python tests/run_all.py mcc_preprocessor,mcc_diagnostics,mcc_object_files,mcc_driver_options,mcc_fuzz`.
2. The QEMU suites, one run at a time (they share `build/esp`): `python tests/run_all.py mcc,mld,manual`. Start with `mcc_on_guest`
   and `mcc_selfhost`: they run the compiler on TouhouOS and exercise the driver thread, folding, the peephole, the inline
   `mem*` code and the merged literals; then `mcc_corpus` (it includes `mem`), `mcc_libgap`, `mcc_kappabox`, `mld_*`,
   `mcc_userland`. When the machine is busy the guest is slow: wait for serial output, do not just raise time-outs.
3. If a guest suite regresses, the history is meant to be bisected: `git revert HEAD` removes the inline `mem*` code and the
   merged literals (a small commit); the one before holds folding, peephole, driver thread, `#if` short-circuit and the new tests.
4. The editors: `bash tools/mcc_build_userland.sh`, then `python tools/mcc_userland_suites.py notebook,sketchbook` (`--none` runs
   the same tests with the clang build for comparison, `--only bin/notebook` replaces one program). Expect `notebook_selftest` to
   pass and its `bench:` line to show 2000 edits in roughly the time of the clang build (800 ms) instead of 20,000 ms. The other
   notebook and sketchbook tests are timing-sensitive GUI tests: compare each failure with the `--none` baseline before blaming
   the compiler.
5. The whole userland against everything: `python tools/mcc_userland_suites.py ""` (about two hours on a loaded machine).
6. Ideas, by value: keep locals in callee-saved registers and avoid the push/pop for simple operands (the biggest speed gain);
   x87/SSE in the inline assembler (then `math.c` and the calculator can use mcc); `#pragma pack`; a real `-S`.
