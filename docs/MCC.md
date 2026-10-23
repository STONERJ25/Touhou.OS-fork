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
| `pp.inc` | the preprocessor: Prosser's hide sets, `#include`/`#include_next`, `#if`, predefined macros, `-E` output |
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
or `rip` address use that address directly, comparisons feed `jcc` without materialising 0/1, and the stack slots of locals in
sibling blocks (and the temporaries of finished statements) are shared, which keeps recursive programs within the stack.
`long double` is `double`.

Calls follow the System V AMD64 convention, so objects made by clang and by mcc mix: integer and SSE registers, structs up to
16 bytes classified per eightbyte, bigger ones in memory (and returned through a hidden pointer), `al` = number of vector registers
for variadic calls, `va_list` with the real 24-byte layout and 176-byte register save area. Thread-local variables use the
local-exec model (`mov rax, fs:0; lea rax, [rax + x@tpoff]`, `R_X86_64_TPOFF32`), which is what `user/libc/src/pthread.c` expects.

Unused `static` functions and variables, and `static inline` functions nobody calls, are not emitted. Strings and `const`
data go to `.rodata`; uninitialised data to `.bss` (tentative definitions are not COMMON).

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
| `mcc_corpus` | QEMU | the 14 programs of `tests/mcc` (arithmetic, pointers, structs, floats, varargs, control flow, statics and TLS, calls, declarators, preprocessor, algorithms, data structures, SHA-256/CRC32, atomics) compiled on the host print the expected text |
| `mcc_on_guest` | QEMU | `mcc` running on TouhouOS compiles and links `tests/mcc/guest/*.c` (stdio, libm, pthreads with TLS and atomics, loopback sockets, fork/pipe/exec/signals), error handling, `-c` followed by `mld` |
| `mcc_selfhost` | QEMU | stage 2 (the compiler built on the host by the gcc-built stage 1), the guest's stage 2 (built by the clang-built `/bin/mcc`) and stage 3 (built by stage 2) are the same file; stage 3 compiles programs correctly |
| `mcc_kappabox` | QEMU | all of kappabox (about 130 applets, 15,000 lines) compiled by mcc passes the 95-line shell test table; the same build also passes the awk, bc, dc, diff and patch suites |
| `mld_relinks_clang` | QEMU | programs relinked by `mld` from clang's objects run like the originals |
| `mcc_libgap` | QEMU | libgap itself (all of it but `math.c`, which uses x87, and `setjmp.asm`), compiled by mcc, links with `utest` compiled by mcc and passes the OS's own user-space suite (libc, fork/exec, signals, mmap, 22 thread sections) |
| `mcc_driver_options` | host | `-D -U -I -include -E -o -c -s -M`, error exits, undefined and duplicate symbols |

The expected output of the programs is the output of the same sources built by gcc against glibc (the corpus is portable C;
`tools/mcc_lintest.sh` does that comparison in WSL), so `.out` files are independent of mcc.

### The host build

The Windows clang on the development machine has no C library, so the host copy of mcc is built with the Linux gcc of WSL:
`bash tools/mcc_host.sh` produces `build/host/{mcc,mld,mar}` (tests/mcc_util.py runs it). `MCC_ASAN=1 bash tools/mcc_host.sh`
builds `mcc-asan` with the address sanitizer. `tools/mcc_lintest.sh FILE.c...` compiles with mcc and gcc, links both against
glibc and compares what they print; `tools/mcc_compile_all.sh` compiles every C file of the OS tree and lists what mcc cannot
digest yet. The programs mcc makes are TouhouOS ELF files: they only run in QEMU (except the differential tests, which link the
objects against glibc).

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

## Limits and how to resume

- Inline assembly: GNU extended asm works (`asm.inc`): operands in registers, memory or immediates, the `a b c d S D` constraints,
  matching operands, named operands, register variables, numeric local labels, and a small AT&T assembler for the integer
  instructions programs use (see `mcc(1)`). x87 and SSE instructions are not understood, so `user/libc/src/math.c` and the
  calculator's engine still need clang; the rest of libgap is built by mcc in the `mcc_libgap` test. File-scope `asm` is
  ignored with a warning.
- Code quality: no register allocation, no constant propagation across statements; programs run perhaps 3 to 5 times slower than
  clang -O2 output. The obvious next steps are keeping locals in callee-saved registers and a peephole pass over the byte buffer.
- Diagnostics stop at the first error; there are no warnings. The parser recovers from nothing.
- `long double` is `double`; there is no x87 code.
- A Windows-native build of the host tools would need a small C runtime shim (no hosted headers are installed); WSL is used instead.
- To add a language feature: parser in `parse.inc`/`expr.inc`, type rules in `type.inc` (`add_type`), code in `gen.inc`, then a program
  in `tests/mcc/` (check it with `tools/mcc_lintest.sh`, then save gcc's output as `NAME.out`).
