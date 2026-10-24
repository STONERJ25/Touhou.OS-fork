# libgap: numerics, stdio, malloc, regular expressions

The C library (`user/libc`) was reworked for exactness and speed in seven steps, each its own commit on branch
`worktree-agent-a503fea82ea373559`. Tests: `tests/case_libc.py` (`python tests/run_all.py libc`).

| Step | Commit | Where | What |
|---|---|---|---|
| 1 | 5b29213 | `src/fpconv.c`, `fpconv.h` | `strtod`/`strtof`/`atof` correctly rounded (any digits, hex floats, inf/nan, subnormals, ERANGE) |
| 2 | b55fe35 | `src/format.c`, `format.h` | printf `%f %e %g %a` exact from the binary value (half-even, any precision), `snprintf` would-be length |
| 3 | cd1bd3c | `src/stdio.c`, `string.c`, `scanf.c`, `scan.h` | stdio moves blocks (memcpy/memchr), one lock per call, SSE2 string functions, new scanf engine over `strtod` |
| 4, 5 | 07263ac | `src/malloc.c`, `stdlib.c`, `include/ctype.h` | segregated-list malloc with in-place realloc, `strtol` family corners, inline ctype |
| 6 | a351782 | `src/math.c`, `include/math.h` | libm in long double with basic ops only, Payne-Hanek reduction, exact integer `pow`, `fma`, the C99 extras |
| 7 | 3b2d19f | `src/regex.c`, `include/regex.h` | `regexec` leftmost-longest, heap stack, memoised search, no `strlen` |

## Design in short

- **fpconv**: Clinger fast path, a 128-bit exact path (19 digits, |e10| <= 27) and big integers (digits past the 800th act as a
  sticky digit); `pack()` rounds with sticky bit, subnormals and glibc's rule for ERANGE. `__fp_digits` produces the exact
  decimal digits of a double for printf.
- **format**: a `Sink` (staging buffer with flush or direct write) is shared by FILE, string and fd output.
- **stdio**: recursive global lock skipped while `__libc_threaded` is 0 (`pthread_create` sets it); `_nl` functions are the
  unlocked bodies; line buffered streams flush up to the last newline (`memrchr`); stderr unbuffered.
- **malloc**: 8-byte boundary tags with a previous-in-use bit (footers only on free blocks), exact-size lists up to 1008 bytes and
  four lists per power of two above, found through a bitmap; mmap blocks grow geometrically; `__malloc_lock` is kept for `fork`.
- **math**: everything in x87 long double with exactly rounded add/mul/div/sqrt only (no `fsin`/`f2xm1`), rounded to double once.
- **regex**: see the header comment of `src/regex.c`. Parser -> AST (lists, arena) -> program (`OP_REP1` for one-byte loops,
  `OP_LOOPINIT/MARK/PROGRESS` for loops whose body can be empty) -> backtracking VM on a heap stack. Every join state
  (instruction, position) is visited once per call unless the pattern has a back-reference; `OP_REP1` loops keep one explored
  interval. `regexec` skips start positions with `memmem`/`memchr`/a byte table and never measures a NUL-terminated string.

## Timings (guest clock, seconds, before -> after; "before" is the ESP built from main d059acb)

stdio on a 6.9 MB file: wc -l 1.25 -> 0.62, sort -n 38.2 -> 14.1, sort 7.1 -> 3.9, sed s/1/2/ 6.3 -> 3.2, grep -c 9 3.6 -> 2.3,
seq 1 1000000 2.9 -> 0.95, tr 2.1 -> 1.2, rev 3.0 -> 1.7, tac 3.1 -> 1.3, awk string grown by one byte 200000 times 108 -> 30.
Regular expressions (1M lines unless noted; includes the stdio gains): grep -c 9 2.00 -> 0.58, grep -c '1.*9' 2.98 -> 0.85,
grep -c '[0-9]*9$' 8.03 -> 1.41, sed s/1/x/g 3.46 -> 1.28, sed s/a/bb/g on one 128 KB line 8.78 -> 0.12, grep 'a.*b' on a 1 MB
line segfault (stack) -> 0.25, awk match(/(a|aa)+$/) on 128 KB segfault -> 0.50, grep -E '(a+)+$' on a^40 b 28.4 -> 0.10,
grep -E '(a|aa)+c' 18.7 -> 0.09. libm: a transcendental call costs about 5 us under QEMU's emulation (was 0.5 us with the x87
instructions); on real hardware a few hundred cycles.

## State when paused

- Everything above is committed and the tree builds with `python tools/build.py` (the one compiler warning is the unused
  variable `hl_ic` in `user/bin/vi/screen.c`, not libc).
- Half done: nothing in the code. Loose ends in the checking:
  - The guest tests for the regex step ran green on the build before three small last edits (unneeded group saves, a second
    `^` in a BRE being literal, `[[:upper:]]` with REG_ICASE). Those edits were checked on the host only (differential test
    against glibc, about 700,000 cases, ASan/UBSan clean). Re-run the guest tests (below) first.
  - `libc_stdio_bigfile` (uses `sed` and `grep` on 1M lines) and `threads` were not run after the regex commit.
  - The host "compile stress" cases (100,000-character pattern, 20,000 alternatives, 99 and 150 nested groups, `((a{100}){100}){100}`,
    `a{32768}`) were not run: the harness had a bug (its buffer for the alternatives was too small) when work was paused. The
    limits themselves (`MAX_NEST`, `MAX_PROG`, `MAX_DUP`, `MAX_STACKED` in `regex.c`) are only reviewed, not exercised.
  - The old-engine timing of `grep -E '(a?){25}a{25}'` was not taken (the new one is 0.08 s).
- **Merged on `cloud/staging` (2026-10-24, commit `5ae8945`).** `src/stdio.c` was hand-merged with main's per-stream locks (each
  `FILE` has its own recursive lock, `__stdio_list_lock` for the list and `fflush(NULL)`, `io_read`/`io_write` record the lock for
  `pthread_cancel`, main's fork hooks), keeping this branch's block-moving `*_nl` functions and the `__libc_threaded` shortcut. After
  the merge `python tests/run_all.py libc,selftest,utest,shell,awk_,bc_,dc_,vi_basic,diff_,patch_,pager,pnote_commands,threads`
  passed (31 tests) on Linux, so the guest tests after the three last regex edits, `libc_stdio_bigfile`, `libc_malloc_patterns`
  and `threads` have now run. The host compile-stress cases (below) are still not run. mcc cannot compile `fpconv.c`
  (`unsigned __int128`), so `mcc_libgap` takes its clang object, as it does for `math.c` (docs/MCC.md).
- Not started: nothing from the task list. Ideas, not started: `\'` and `` \` `` in regex, equivalence classes beyond one
  character, POSIX "longest subexpression" rules for submatches.

## Tests

`python tests/run_all.py libc` runs: `libc_strtod_order`, `libc_strtod_roundtrip`, `libc_printf_float_matrix`, `libc_printf_misc`,
`libc_seq_float`, `libc_stdio_bigfile`, `libc_malloc_patterns`, `libc_strtol_family`, `libc_math_awk`, `libc_regex_longest` (70 patterns
on all 364 strings up to length 5, through awk's `match()`, expected values from a Python leftmost-longest oracle),
`libc_regex_tools`, `libc_regex_big` (1 MB line, nested loops, `s///g` on a long line). Regression set for libc changes:
`utest`, `shell`, `awk_checks`, `bc_language`, `vi_basic`, `diff_formats`, plus `pager` and `pnote_commands` for regex; `threads`
once at the end. Run one QEMU test session at a time (they share `build/esp/cmdline.txt`).

The host checks live outside the repository (in the session scratchpad, `scratchpad/h/`): they compile libgap's sources with
`-ffreestanding -nostdinc -I user/libc/include`, rename the symbols to `gap_*` (`-D` or `objcopy --redefine-syms`), and link them
next to glibc to compare results on random input (strtod, printf, stdio, string, scanf, malloc, math, regex). Rebuilding such a
harness takes a wrapper per library and a random generator; run with `LC_ALL=C`. glibc is wrong in two places the regex
harness found (`x*\B` skips an empty match; a `$` in a REG_STARTEND window with REG_NEWLINE): Python's `re` was used to decide.

## Known limits

- Math: `pow` with a non-integer exponent and integer powers that need more than 128 bits are within 1 ulp but not always
  correctly rounded (about 0.5 percent); libm is slower than the old single x87 instructions.
- Regex: submatches among equal-length matches follow backtracking priority, not POSIX "longest first subexpression" (`(a|ab)(c|bcd)(d*)`
  on `abcd` gives \1 = a, glibc gives ab). Patterns with back-references have no memo: exhaustive search capped at 20M steps per start
  position. A match that needs more than 4M stack frames (a loop over a group, about 4 frames per character with captures and
  2 without, so over a million characters of one line) returns REG_ESPACE, which the tools read as no match. Bytes only (no
  multibyte locales).
- scanf differs from glibc on incomplete numeric prefixes (gap gives the characters back) and `%3c` with short input fails.
- `long double` is double everywhere (`strtold`, printf `%Lf`).

## How to resume

1. `python tools/build.py`, then `python tests/run_all.py libc_regex,libc_stdio_bigfile,libc_malloc_patterns`, then the regression
   set: `utest,shell,awk_checks,bc_language,vi_basic,diff_formats,pager,pnote_commands`, then `threads`. Fix anything that
   fails in `user/libc` (a failure in the regex tests after the three last edits points at `savelim` in `exec_at`/`run_init`,
   the `first` handling in `parse_cat`, or the class folding in `parse_bracket`).
2. Rebuild the host regex harness (see above), fix its alternatives buffer (`20000 * 8 + 10`), run the compile-stress cases
   under `ulimit -s 256` and ASan.
3. Optional follow-ups for other owners: `sed.c` (`do_subst`) and `grep.c` (`find_match`) call `regexec` on `line + pos` with
   REG_NOTBOL instead of REG_STARTEND on the whole line, so `\<`, `\b` and `\B` lose the character before `pos` (`echo aa | sed
   's/\<a/X/g'` gives XX); `awk.c` `scan_num` and integer `^` do not use libc (awk's `2^-1074` is 0, `(1.0000001)^100000` is
   inexact); `sed.c` `buf_append` grows by exact fit; `bytes.c` (`expr :`) tries every start position instead of anchoring.
4. Docs that still describe the old regex engine and should be updated by their owners: `docs/AWK.md` (known limits), `docs/NEXT.md`
   (line about the recursive leftmost-first backtracker), `docs/UTILITIES.md` (awk row and the note about leftmost-first
   semantics).
