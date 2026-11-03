# Self-hosting: TouhouOS builds TouhouOS

The aim: the system rebuilds itself without a host PC. The host today (`tools/build.py`) uses LLVM, NASM and Python; this page records how far
the system's own tools have come: a C compiler (`mcc`, with `mld` and `mar`), a `make` (`mmake`) and an assembler (`mas`). Written 2026-11-03 by
the self-hosting agent; the compiler itself is described in [MCC.md](MCC.md).

## Where it stands

| Stage | State | Proof (test, how to run) |
|-------|-------|--------------------------|
| M1 mcc on today's tree | done: the parked branch (squashed) builds and its tests pass in QEMU | `mcc_corpus mcc_on_guest mcc_selfhost mcc_kappabox mld_relinks_clang mld_got_relocations mcc_libgap manual`; host: `mcc_preprocessor mcc_diagnostics mcc_object_files mcc_driver_options mcc_fuzz` |
| M2 userland built by mcc | done but for the calculator (80-bit `long double`); the 12 failed GUI tests of the parked branch pass | `python tools/mcc_userland.py`, then `python tools/mcc_userland_suites.py notebook,sketchbook` (28 of 28 pass); wider run below |
| M3 the OS rebuilds its userland on itself | done: `make` in a guest rebuilds libgap, kappabox, mmake, mcc and mas, the result runs and a second guest boots with the rebuilt kappabox | `selfhost_rebuild_userland`, `selfhost_mmake_guest`, `selfhost_touch_fat` |
| M4 an assembler | done: `mas` writes the very same ELF objects as NASM 3.02 for the nine `.asm` files and for a 12-file corpus | `python tools/hosttest.py discover -s tests/host -p "test_mas.py"`, `selfhost_mas_guest` |
| M5 the kernel by mcc + mas + mld | started: all 90 C files of `kernel/` and `shared/` compile; not linked, not booted | `bash tools/mcc_kernel.sh` (through `wsl.exe -e`), `test_mcc_kasm.py`; the gaps are listed below |

## Reproducing each stage (Windows PC with WSL's Ubuntu, as `the project's internal briefs` describes)

```
python tools/build.py                                   # the normal (LLVM) build; the initrd now carries /ScarletDevilMansion/src
python tools/mcc_userland.py                            # the whole userland by the host mcc (about 10 s): build/mcc-user/{bin,sbin}, report.json
python tools/qemu-slot.py -- python tests/run_all.py mcc_,mld_,selfhost_            # the guest tests
python tools/qemu-slot.py -- python tools/mcc_userland_suites.py notebook,sketchbook  # any suites against the mcc-built programs (--none: the clang baseline)
python tools/mcc_compare.py shell,utest                 # both, one after the other, and a table of where they differ
python tools/hosttest.py discover -s tests/host -p "test_mmake.py"   # mmake against GNU make (66 Makefiles)
python tools/hosttest.py discover -s tests/host -p "test_mas.py"     # mas against NASM
python tools/hosttest.py discover -s tests/host -p "test_mcc_kasm.py"
wsl.exe -e bash tools/mcc_bench.sh                      # speed of mcc's code against gcc
wsl.exe -e bash tools/mcc_kernel.sh                     # which kernel files mcc can compile
```

Inside a TouhouOS guest (nothing here needs the host):

```
cd /ScarletDevilMansion/src
make OUT=/tmp/build            # libgap, kappabox, mmake, mcc, mas: about a minute in QEMU without KVM
make OUT=/tmp/build install DESTDIR=/tmp/newroot
make -n                        # what would be rebuilt
mas -o crt0.o libc/crt0.asm    # the assembly sources are built by mas when /bin/mas exists
```

`/ScarletDevilMansion/src` holds the sources of libgap (`libc/`), kappabox, mmake, mas and mcc, the `Makefile` (from `user/bin/mmake/selfhost.mk`) and
`gen-applets.awk` (which writes the applet table that `tools/build.py` writes on the host). The libraries other than libgap (`libkappa.a`, `libfairy.a`, ...)
and the headers come from the system root next to it; rebuilding them is the same recipe as the host tool's (`tools/mcc_userland.py`), they are not in the
guest Makefile because the command-line system does not need them.

## The tools

- **mcc / mld / mar** ([MCC.md](MCC.md)): C11 subset to static ELF, linker, archiver.
- **mmake** (`user/bin/mmake/mmake.c`, manual `mmake(1)`, linked as `make`): variables with all assignment flavours, `$(...)` functions, conditionals,
  `define`, `include`, pattern and static pattern rules, `.PHONY`, order-only prerequisites, automatic variables, `-f -C -n -s -k -i -B -j`. 66 Makefiles run through
  GNU make 4.3 and mmake give the same output and files (`tests/host/test_mmake.py`, with AddressSanitizer). Left out: parallel jobs, suffix rules, target-specific
  variables, `vpath`, `$(eval)`, secondary expansion, deletion of intermediate files. Differences found on the way and kept: a command without shell syntax is
  started directly, as GNU make does (the kernel limits one `exec` argument to about a kilobyte, so `sh -c "<a link line with 50 objects>"` fails with
  E2BIG); a missing program falls back to `/bin/sh`.
- **mas** (`user/bin/mas`, manual `mas(1)`): NASM-syntax x86-64 assembler: preprocessor (`%macro %rep %assign %define %if %include`, `%+`, `%%labels`), expressions,
  sections and attributes, `global/extern/equ/db..dq/resb../incbin/align/times`, the integer instruction set of insn.inc with NASM's encoding choices
  (branch relaxation, `83 /r` immediates, accumulator forms, `mov r32, imm32`). The output is NASM's layout byte for byte (ELF header, section
  headers, `.symtab` with locals before globals, `.strtab` in symbol creation order, `.rela.*`, padding to 16). `-g -F dwarf` are accepted and ignored (no debug sections).
  Left out: floating point, SSE/AVX, segment and control registers, `bits 16/32`, `org`, other object formats.

## File times (the "check utimes and the clock" item)

- `touch` did not touch: the old applet ignored every option and never changed an existing file's time, so `make` could not be told that a file changed.
  It now calls `utimes()`: `-c -a -m -t [[CC]YY]MMDDhhmm[.ss] -d DATE -r FILE`, and plain `touch f` sets the modification time to now (`user/bin/kappabox/fileops.c`, the one
  applet changed outside the self-hosting files).
- `utimes` works on tmpfs and on FAT (`selfhost_touch_fat` reads the directory entries back with the host's FAT reader: FAT keeps even seconds, 1980-2107).
- Times are whole seconds everywhere. mmake rebuilds when a prerequisite is **newer** (greater), so a file edited in the same second its target was built looks
  unchanged: tests use `touch -t` with explicit times instead of relying on the clock, and the manual page says so. Targets built in one run are always newer than
  their inputs whatever the clock says. With a stale clock (the Dell's RTC says 2025) objects get a time older than the sources' mtimes from the image, so the
  next `make` rebuilds everything once more: wasteful, never wrong.

## mcc against the LLVM build

- **Size** (stripped, 45 programs of user/bin and user/sbin, the calculator excluded from the ratio's meaning because it is the clang build): 13.3 MB against 7.8 MB, **1.71 times**;
  `kappabox` 1.25 MB against 0.73 MB.
- **Speed** (`tools/mcc_bench.sh`, host-native, checksums identical in all compilers; ms, lower is better):

  | kernel | mcc | gcc -O0 | gcc -O2 | mcc / -O2 |
  |--------|-----|---------|---------|-----------|
  | quicksort 1.5 M ints | 300 | 166 | 98 | 3.1 |
  | sieve to 20 M | 190 | 138 | 73 | 2.6 |
  | SHA-256 of 24 MiB | 1637 | 214 | 63 | 26 |
  | Mandelbrot 400x300 | 115 | 28 | 10 | 11.5 |
  | hash table, 400 k formatted keys | 60 | 58 | 42 | 1.4 |

  The accumulator code generator keeps every operand on the stack and never inlines (`rotr` is a call): fine for most programs, painful for hot arithmetic loops.
- **Compile speed**: the host build of the whole userland (700 files) takes 10 s on 14 threads; in a guest (QEMU, no KVM) libgap + kappabox + mmake + mcc + mas rebuild in about a minute.
- **Tests**: the parked branch's 12 failed GUI tests (8 notebook, 4 sketchbook) pass: `notebook` and `sketchbook` 28 of 28 with the mcc-built programs. See "Wider run" for the rest.

## What was found and fixed on the way

| Where | Finding |
|-------|---------|
| `tests/case_mcc.py` | `mcc_selfhost` compared an executable with a file it never wrote; the compiler's stage 2 = stage 3 claim itself holds in QEMU (also with the newest edits) |
| mcc | `const struct S *p` (or a typedef name, or `volatile`) written before `struct S` is defined kept an incomplete copy of the type: `p->ops->lookup(...)` was "member access in an incomplete type" (six kernel files, `vfs.h`). Fixed, test `tests/mcc/incomplete.c` |
| mcc | inline assembly: SSE moves/operations and x87 with the `t`/`u` constraints (libm, the compositor's copy loop); `fldt`/`fstpt` stay refused on purpose (mcc's `long double` is a double) |
| gcc vs x87 asm | `fscale` with a `"st(1)"` clobber leaks an x87 stack slot per call under gcc (NaN after eight calls); mcc empties the FPU stack after each statement that touches it and does not |
| kappabox `touch` | see "File times" |
| kernel `exec` | one argument longer than about 1 KB fails (`Argument list too long`): the hardening branch raises the limits; mmake works around it |

## Known gaps of the toolchain

- mcc: no VLAs, `_Complex`, `alloca`, computed `goto`, K&R definitions; `long double` is a double (the calculator needs the real 80-bit type and x87 code generation,
  `fldt`/`fstpt` in asm are refused); no register allocation; diagnostics stop at the first error; `-S` is not supported; `#pragma pack` is ignored.
- The calculator (`user/bin/calculator`) is the one program still built by clang, and so is the code of two libraries: libtls (`unsigned __int128`) and **libjs** (added by the JavaScript engine's merge on 2026-11-03: its headers use `__builtin_signbit`,
  which mcc does not know, and `vm.c` dispatches with computed `goto`, which mcc does not have; a `switch` fallback under `#ifdef __MCC__` in vm.c would do). `mcc_userland` lists both as allowed fallbacks.
- mmake and mas: see above.
- The host tools (`mcc_userland.py`, the tests) need WSL or Linux with gcc, because the host mcc is a Linux program.

## M5: the kernel by mcc + mas + mld (what is known, what is missing)

Done: `bash tools/mcc_kernel.sh` compiles every C file of `kernel/` and `shared/` (90 files, kernel flags `-DKERNEL=1 -nostdinc`) without error. The inline assembly the kernel
uses is in (`rdmsr wrmsr wbinvd lgdt lidt ltr invlpg clflush fxsave fxrstor ldmxcsr rdrand lretq`, `mov` to and from `%cr0..%cr15` and the segment registers, `label(%rip)`,
the `%c` modifier); `tests/host/test_mcc_kasm.py` shows mcc's bytes for 60 such statements are gcc's. The seven kernel `.asm` files assemble with `mas` identically to NASM.

Missing before a kernel can be linked and booted (in the order they will bite):

1. **Custom sections**: `__attribute__((section(".initcall")))` and friends are ignored by mcc, so the `INITCALL` tables, `ksyms` and any other linker-script input sections would be empty. mcc's output sections are the fixed six (`x86.inc`): named sections need `obj.inc` support.
2. **`-mgeneral-regs-only`**: a variadic function's prologue saves `%xmm0-7` (`movups`), and `float`/`double` code uses SSE; the kernel must never run SSE. mcc needs a switch that omits the saves and rejects floating point.
3. **A kernel link mode in mld**: `kernel/linker.ld` (higher-half addresses, section order, `PROVIDE`d symbols such as the section bounds, `.ksyms` last, program headers) has no equivalent in mld, which lays out user programs only. `tools/build.py` also needs `ksyms.bin` from the linked image (a two-pass link, as now).
4. **Code model**: mcc uses RIP-relative references and `PC32`/`PLT32` for everything; that is fine inside the kernel's top 2 GiB, but nothing has checked that the loader's mapping and `-mno-red-zone` assumptions (interrupt stacks) hold for mcc's frames.
5. **Debugging a kernel that does not boot** has no serial log until early init: the first steps are `kmain` printing, then `initcall`s; test in QEMU with `selftest` and `hwaudit_stage_marks`.

Plan: (1) sections in the object writer and in mld, (2) the switch, (3) a `-T`-like kernel layout in mld driven by a small spec instead of a linker-script parser (the script is 40 lines), (4) build the
kernel with `tools/build.py --cc mcc` style options for one directory at a time (`lib/`, `mm/`, ...) next to clang objects, so that a failure is localised, (5) the full kernel, (6) boot test `selftest,utest` on it.

## What the last runs showed (2026-11-03, on `agent/selfhost` merged with `main` at 50e4985)

| Run | Result |
|-----|--------|
| core, QEMU through the limiter: `mcc_ mld_ selfhost_ manual` | all 16 `mcc_`, `mld_`, `selfhost_` tests PASS (two needed their own test fixed first: `mcc_corpus`, whose x87 test relied on `f2xm1` out of range, and `mcc_kappabox`, whose library list predated libtls/libhttp). `manual` FAILS on `main`'s `vncd.pw:14` (verbatim line 102 columns, limit 78), not on anything of this branch (my `mas.pw` had the same problem and is fixed) |
| `mcc_userland`: the whole userland built by mcc (45 programs but the calculator; libgap's `crt0.o`/`setjmp.o` from mas) boots the desktop, passes `utest` and the 95-line shell table | PASS (58 s) |
| `mcc_libgap`: libgap compiled by mcc links with `utest` and passes the OS's own user-space suite | PASS |
| `notebook` + `sketchbook` (28 GUI tests) with the mcc-built programs (before the merge of `main`) | 28 of 28 PASS; the parked branch had failed 12 of them |
| the focused comparison on the merged tree (`notebook sketchbook vi awk bc diff` with the mcc-built programs, `tools/mcc_userland_suites.py`) | at the time of writing: `bc_language`, `bc_mathlib` (libm by mcc's x87 asm), `awk_big_input`, `diff_formats`, `diff_dirs` PASS; `awk_checks` and `bc_dc_manual_pages` fail only because of the two over-wide manual lines above (`patchouli --check-all`); the GUI part of the run had not finished, see the log in `build/logs/final-compare-mcc.log` if it is still on disk |
| host: `test_mmake` (66 Makefiles against GNU make 4.3), `test_mas` (9 project files + 12 corpus files byte-identical with NASM 3.02), `test_mcc_kasm` (60 statements byte-identical with gcc), `mcc_preprocessor mcc_diagnostics mcc_object_files mcc_driver_options mcc_fuzz` | all PASS |
| `bash tools/mcc_kernel.sh` | 90 of 90 C files of `kernel/` and `shared/` compile |

**Not compared**: the other ~120 userland tests (explorer, media, games, danmaku, accounts, ssh, audio ...) were not run against the mcc-built programs on the merged tree. A first slice of that
wide run (12 tests: accounts, apps, audio) passed before it was stopped to merge `main`. The command is above (`tools/mcc_compare.py FILTER`); the whole thing is about two hours on a loaded PC.

A fix that came from the merge of `main`: the new library **libtls** uses `unsigned __int128` (in `tlsint.h` and for real in `bn.c`), which mcc does not have, so its 17 files stay clang objects in the mcc
userland (`report.json`: `fallback`). Only `bn.c` needs the type for arithmetic; the other 16 would compile if mcc accepted the typedef and refused the arithmetic (an incomplete type named `__int128`: ten lines in `declspec`).

## State when paused / how to resume

State: branch `agent/selfhost` holds everything above, committed and merged with `main` (50e4985). Nothing is half-done on disk. The parked branch's claim (stage 2 = stage 3) holds; its newest edits
ran in QEMU for the first time here and passed; the old GUI failures were the merged string literals and the inline `mem*` expansion, both already in that branch.

To resume, in this order:

1. `python tools/build.py`, then `python tools/mcc_userland.py` (10 s) and look at `build/mcc-user/report.json`: `fallback` should list `bin/calculator` and the `user/lib/tls/src` files only.
2. `python tools/qemu-slot.py -- python tests/run_all.py mcc_,mld_,selfhost_,manual` (about 10 minutes): all pass. (`mcc_corpus` and `mcc_kappabox` were fixed after the core run of the merged tree: the first for an x87 test that
   depended on QEMU's handling of `f2xm1` out of range, the second for a hard-coded list of libraries; rerun them first.)
3. The wide comparison: `python tools/mcc_compare.py "notebook,sketchbook,explorer,media,diff,jobctl,accounts,login,pnote,image,bc,vi,threads,snake,kshell,desktop,danmaku,awk,apps,utest,shell,patch,pager,minesweeper,lock,games,dc,shrine,hidpi,audio,sshcrypto,etc_,time_" --retry-failed`.
   A test that fails with mcc and passes with clang is a compiler bug until proved otherwise; the harness overrides a test's own ESP (`tools/mcc_userland_suites.py` replaces `esp=` for every guest), so
   tests that bring their own initrd files (media, some ssh) fail in both columns and mean nothing there: change it to leave a test's own `esp` alone.
4. Then, by value: (a) `__int128` as a refused type so that libtls compiles (above); (b) the calculator needs a real 80-bit `long double`; (c) the speed of mcc's code (SHA-256 is 26 times slower than gcc -O2): keep scalars in
   callee-saved registers, inline `static` one-liners like `rotr`; (d) M5, in the order of the list above (named sections first); (e) build the other libraries inside the guest (`libkappa`, `libfairy`, ...: the same recipe as the
   host tool) so that the whole userland, not only the command-line part, rebuilds on the OS; (f) `mmake`: parallel jobs would help on a machine with KVM.
5. Things to tell the owner: `touch` was a no-op for existing files (fixed in `user/bin/kappabox/fileops.c`, the one applet changed outside the self-hosting files); one `exec` argument is limited to about 1 KB in the kernel on `main`
   (mmake avoids `sh -c` for plain commands because of it); the Dell's stale clock makes `make` rebuild everything once after each boot's first build (never wrong).
