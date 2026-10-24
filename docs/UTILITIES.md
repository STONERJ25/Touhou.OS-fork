# Utilities: what exists, what does not

This page tracks the work "close the gap between this userland and a comfortable Unix command line". It says plainly what is
done, what is only partly done and what was never started. Details of the bigger tools live in their own pages:
[AWK.md](AWK.md), [DIFF_PATCH.md](DIFF_PATCH.md), [BC_DC.md](BC_DC.md), [VI.md](VI.md), and (on an unmerged branch, see below)
LIBC.md. The manual pages are in `initrd/ScarletDevilMansion/share/patchouli/1/` (see [TOOLS.md](TOOLS.md)); every tool listed
as done has one. The kernel half of job control is in [JOBCONTROL.md](JOBCONTROL.md).

## State when paused (second pause, 2026-10-23 evening)

| Item | State |
|------|-------|
| awk, diff, patch, bc, dc | **done**, merged (first pause) |
| vi | **mostly done, merged**: about 950 scripted guest checks plus screen tests passed at the last full run; differential test against the host's vim is half done (see below) |
| libgap numerics and speed (exact `strtod`/`printf`, block-wise stdio, new malloc, libm, leftmost-longest regex) | **done on its own branch, NOT merged**: it conflicts with main in `user/libc/src/stdio.c` and was not fully tested (see below) |
| gapsh job control | **not started** (the kernel half is in main) |
| gapsh audit (here-documents, `set -e`, `trap`, ...) and the shell conformance test | **not started** |
| the small new tools (`join split csplit fmt column unexpand tsort timeout time watch nohup install killall pgrep vmstat fuser getopt du sha1sum` and standalone `printf`, `test`/`[`, `pwd`) | **not started** (no helper was launched for them) |
| extensions of the weak applets (`ls find grep sed sort cut paste tr uniq head tail wc xargs tee cp mv rm chmod chown stat date od xxd base64 md5sum ...`) | **not started** |

The branch head builds (`python tools/build.py`). Nothing was pushed.

### Done and merged

| Tool | Where | State |
|------|-------|-------|
| `awk` | `user/bin/kappabox/awk.c` | Complete POSIX awk (about 3,200 lines): all pattern and statement forms, fields, `FS`/`RS` incl. paragraph mode and regex `RS`, arrays, all `getline` forms, redirections and pipes, user functions, every POSIX builtin plus `systime`/`strftime`. Tests `awk_checks` and `awk_big_input` pass. Limits: byte-counted strings, no `gensub`/`asort`/`IGNORECASE`; `scan_num` and integer `^` bypass libc (so `2^-1074` is 0). |
| `diff` | `user/bin/kappabox/diff.c` | Myers diff with the GNU pipeline: normal, `-u`, `-c`, `-q`, `-s`, `-r`, `-N`, `-a`, `-b -w -i -B`, `-x`, `--label`, `--strip-trailing-cr`, directories, binary detection, exit 0/1/2; compared with GNU diff 3.12. No `-e -y -p -I --color`. Header times end in `.000000000` (the kernel's `stat` has whole seconds only). |
| `patch` | `user/bin/kappabox/patch.c` | Unified, context and normal diffs, several files per patch, `-p -d -i -o -R -E -b -s --dry-run`, exact matching with offset search, `.rej` files, create/delete via `/dev/null`. Not done: `-N`/reversed-patch detection (accepted, does nothing); fuzz is deliberately absent. |
| `bc` | `user/bin/kappabox/bc.c`, `bignum.c` | Full bc language, `-l` math library, GNU line wrapping and errors, arbitrary precision (base 10^9 limbs, no Karatsuba). |
| `dc` | `user/bin/kappabox/dc.c` | All standard commands, registers and arrays, macros with tail calls, GNU messages and wrapping. |
| `vi`, `vim`, `view` | `user/bin/vi/`, `vilinks.c` | See below. `$EDITOR` stays `pnote`; `vi` is for people who want vi. |

### vi (merged at abe5159; details and a "State when paused" section in [VI.md](VI.md))

* **Done and checked in the guest:** motions with counts, operators (`d c y > < ! = gu gU g~ g? gq gw J gJ`, forced `v V Ctrl-V`),
  text objects, registers, dot repeat, macros, marks and a jump list, undo/redo/`U`, insert-mode keys, visual modes, ex ranges and
  commands (`:s` all flags, `:g`/`:v`, `:m :t :j :normal :sort`, filters, `:r`, `:w !cmd`, `:set`), mappings and abbreviations,
  start-up options, odd files (empty, no final newline, CRLF, UTF-8, invalid bytes, read-only), the screen (status line, ruler,
  scrolling, wrapping, tall lines), serial and desktop-terminal key decoding, exit paths and signals. A heap overflow on the
  first edit of a long line was found and fixed. A 100,000-line file: the listed operations take 0.3-2 s, except `:g/5/m0`
  (60 s).
* **Test results for vi** (from the vi helper, on its own branch before the merge): `vi_manual vi_basic vi_motion_basic
  vi_motion_words vi_motion_search vi_edit_operators vi_edit_registers_dot_macros vi_edit_undo_insert vi_ex_lines
  vi_ex_subst_global vi_ex_files_options vi_startup_and_files vi_screen_basic vi_screen_scroll vi_screen_long_lines
  vi_screen_keys_serial vi_exit_paths vi_textobjects vi_visual_modes` passed ("19 tests, 0 failed"), before the helper's last
  fixes. `vi_big_file` failed there only on two wrong expectations (fixed afterwards, not re-run); `vi_screen_interrupt` was never
  run; `shell` and `manual` were not run on the vi branch. On the merged head only the build was checked.
* **Half done:** a differential test against the host's real vim 9.2 (`tests/host/vi_diff.py`, `vifuzz.c`: vim on the Windows
  host produces the expected answers, vi runs the same cases in the guest; commands in VI.md): about 4,800 generated cases ran
  without a hang or crash; 47/800 motion, 266/2500 operator and 97/1500 text-object cases differ from vim. Seven groups of differences are written down in VI.md with vim's rules (count overshoot
  for `{ } ( ) [[ ]]`, `]]` cursor placement, `/pat/+N` column, `gj`/`gk`, `gq` details, sentence/bracket objects, forced
  `cv`/`dv`). None are fixed yet. The "seq" and "ex" generators exist but were not run.
* **Not started:** `\v \V \zs \%(` in patterns, `/foo\nbar` searches, `gj gk g0 g$`, Ctrl-K digraphs, `:undo N`, the `"=` register.
  Limits: patterns are byte based, windows/folds/syntax colours do not exist.

### The libc branch (NOT merged)

Branch `worktree-agent-a503fea82ea373559`, head `3a99e04`, worktree `.claude/worktrees/agent-a503fea82ea373559` (keep it until
the merge is done). Seven commits, all on top of the old `d059acb`; they build there. Notes and a "State when paused" section are
in `docs/LIBC.md` **on that branch**.

* strtod/strtof/strtold exactly rounded (`fpconv.c`); printf `%f %F %e %E %g %G %a %A` exact (`format.c`); stdio moves data in
  blocks; SSE2 string functions; a scanf engine over the new strtod (`scanf.c`); a segregated-list malloc with in-place realloc;
  an accurate libm with the missing functions; a leftmost-longest `regexec` with a heap stack, a memoised search and no
  `strlen` (cut a 128 KB `sed s/a/bb/g` from 8.8 s to 0.12 s, and `grep 'a.*b'` on a 1 MB line from a segfault to 0.25 s).
  Guest timings before -> after (seconds): `wc -l` 1.25 -> 0.62, `sort -n` 38.2 -> 14.1, `sort` 7.1 -> 3.9, `seq 1e6` 2.9 -> 0.95.
* **Why it is not merged:** merging it into current main conflicts in `user/libc/src/stdio.c`. Main's commit `f618cdf` gave
  every `FILE` its own recursive lock (`__stdio_list_lock`, `io_read`/`io_write` wrappers that record `stdio_blocked` for
  `pthread_cancel`, locks taken in library order around `fork`), while the libc branch keeps one global `__stdio_lock` and a
  `__libc_threaded` flag. All other files merge automatically (`pthread.c` merges textually; check it by hand). The resolution is:
  take the libc branch's block-moving `*_nl` functions, but lock `f->lock` per stream as main does, send every `read`/`write`
  in `fill`, `write_all` and the direct-read path through `io_read`/`io_write`, keep `__stdio_list_lock` for the stream list and
  `fflush(NULL)`, and keep main's fork handling. Then run the tests listed under "How to resume".
* **Tests the libc helper ran:** green on its branch: `bc_language diff_formats vi_basic` after the libm commit; `apps_cli
  libc_math_awk utest awk_checks shell`; with the new regex (before its last three small edits) `utest shell awk_checks
  pnote_commands pager vi_basic` and `libc_regex_longest libc_regex_tools libc_regex_big`. **Not run:** `threads`
  (never), `libc_stdio_bigfile`, `libc_malloc_patterns`, `bc_language` and `diff_formats` after the regex commit, and any guest
  test after the last three regex edits (group saves skipped when `nmatch` is small, BRE `^^` literal, `REG_ICASE
  [[:upper:]]`; those were checked on the host against glibc only). Known limits: `pow` with non-integer exponents can be 1 ulp
  off; regex submatches follow backtracking priority, not POSIX longest-first; back-reference patterns have no memo.

### Not started

Nothing below has been written; the existing versions of the existing tools are unchanged.

* **gapsh job control** (do this first; read [JOBCONTROL.md](JOBCONTROL.md), "What the shell has to do"). Today gapsh calls
  `waitpid` without `WUNTRACED`, so Ctrl-Z stops the foreground job and the shell keeps waiting. To do in `user/bin/gapsh/exec.c`
  and `builtins.c`: ignore `SIGTSTP`/`SIGTTIN`/`SIGTTOU` in the shell, one process group per job with `setpgid` in parent and
  child, default signals in the child before `exec`, `tcsetpgrp` to the job for the foreground and back to the shell after
  (also after a stop), `waitpid(..., WUNTRACED)`, `fg` = `tcsetpgrp` + `kill(-pgid, SIGCONT)`, `bg` = `kill(-pgid, SIGCONT)`,
  collect background changes before each prompt with `waitpid(-1, WNOHANG|WUNTRACED|WCONTINUED)`, `jobs`, job specs
  (`%n %% %+ %- %name`) for `fg bg kill wait`, `disown`, and the `[1]+  Stopped  cmd` / `[1]+  Done  cmd` notices. Test it with
  ptys as `tests/case_jobctl.py` and `user/bin/jctest.c` (kernel side) do, plus a shell-level case in the conformance test.
* **gapsh audit** (nothing changed; `user/bin/gapsh/`): `<<-` here-documents are not parsed (the lexer makes `-EOF` the
  delimiter); `set -e -u -x -f -o pipefail` are accepted and ignored; `trap` handles only `EXIT` and the `''`/default signal
  dispositions (no handler strings for signals); `wait` takes one pid and no job specs; `exec` with only redirections does
  nothing lasting; no `printf -v`; `read` lacks `-s -n -d -t`; `test` lacks `-ef` and the `-a`/`-o` edge rules; arithmetic lacks
  `,` and `**` and some assignment operators; a script read from a pipe is joined by an O(n^2) loop in `main.c`. Found by the
  vi helper: `"$@"` with no arguments expands to one empty word (it must expand to nothing), and the `printf` builtin needs `\0NNN`
  (a leading 0) while `\NNN` stays literal. The conformance test `tests/case_shell_conform.py` (about a dozen scripts with
  expected output, run in the guest) does not exist yet.
* **New tools:** `join`, `split`, `csplit`, `fmt`, `column`, `unexpand`, `tsort`, `timeout`, `time`, `watch`, `nohup`,
  `install`, `killall`, `pgrep`, `vmstat`, `fuser` (`/proc/<pid>/fd` exists now), `getopt`, `du`, `sha1sum` (and `sha512sum`),
  `cksum`, `stty`, `tput`, `pwd`, a standalone `printf`, and standalone `test` and `[` applets (so `find -exec [ ... ]` works).
  `nice`/`renice` already exist (main), as do `df`, `truncate`, `gzip`, `tar`, `zip`: do not rewrite them.
* **Weak existing tools to extend** (not touched): `factor` still does trial division, so a large 64-bit prime takes minutes
  (needs Miller-Rabin and Pollard rho); `shuf`, `seq`, `mktemp`, `realpath`, `strings`, `hexdump` (only `-C`), `xxd -r`, `fold`, `nl`,
  `expand`, `sort` (`-k` ignores character offsets, no `-h -V -M -m`), `uniq`, `cut`, `tr`, `paste`, `wc`, `xargs`, `ls`, `find`,
  `grep`, `sed`, `stat -c`, `date -d`, `touch` (can use `utimes` now), `cp/mv/rm/chmod/chown` option sets,
  `md5sum`/`sha256sum`.

### Known problems and requests collected from the helpers

* `sed.c` `do_subst` and `grep.c` `find_match` call `regexec` on `line + pos` with `REG_NOTBOL`, so `\<`, `\b` and `\B` lose
  the previous character (`echo aa | sed 's/\<a/X/g'` prints `XX`; it should print `Xa`). Use `REG_STARTEND`. Also: `sed`
  `buf_append` grows with exact fit; `expr :` tries every start position.
* libgap (when the libc branch is merged, re-check these): the regex engine is byte based, has no non-capturing groups or
  non-greedy repeats; `memmove` with `dst > src` copied backwards with `std; rep movsb` (very slow under QEMU; check whether the
  libc branch's string functions fixed it); `qsort` misbehaves with inconsistent comparators; `alarm()` is a stub.
* kernel: user-mode `SIGSEGV` handlers apparently never run (the process is killed first, so a program cannot print its own
  crash message). `ptlib`: `pt_active` is static per translation unit, so `pt_oom()` in another file does not restore the terminal.
* Tests: `bc_language` failed 1 of 169 checks on the merged main (before any helper merge): `echo '7' | bc /dev/null` printed
  the kernel log line `[Eiki] 'gapsh' (pid 307) was exorcised by signal 13` into the captured output. `bc` exits without reading
  its stdin, so the `echo` stage dies of SIGPIPE and the kernel logs it to the console; whether the line appears is a race, so
  the check is flaky rather than the bc wrong. The same line can show up for any pipeline whose reader exits early.
* The full suite (`python tests/run_all.py`, about 25 minutes) was **not run** on this branch.

### How to resume

1. `python tools/build.py` (6-25 s). Never run two test runs in one checkout at once.
2. Run the cheap checks first, one run at a time: `python tests/run_all.py vi_manual,manual` (the filter takes comma-separated
   substrings), then `shell`, then `vi_` (about 20 tests), then `awk_ diff_ patch_ bc_ dc_`.
3. Merge the libc branch (see "The libc branch"): resolve `stdio.c`, build, then run
   `libc_regex,libc_stdio_bigfile,libc_malloc_patterns`, `utest`, `shell`, `awk_checks`, `bc_language`, `vi_basic`,
   `diff_formats`, `pager`, `pnote_commands`, and `threads` last. If something breaks, bisect on the seven libc commits.
4. gapsh job control, then the rest of the gapsh audit with `tests/case_shell_conform.py` (gapsh is shared: one owner at a
   time), then the small tools and the extensions in this order: `sed`/`grep` `REG_STARTEND` fix, `touch` with `utimes`,
   `du`, `printf`/`test`/`[`/`pwd` applets, `timeout`, `time`, `nohup`, `watch`, `pgrep`/`killall`, `join`, `split`, `csplit`,
   `fmt`, `column`, `unexpand`, `tsort`, `sha1sum`, `fuser`, `getopt`, then the option sets of the weak applets. Each new
   applet needs a manual page (`share/patchouli/1/NAME.pw`, lines at most 78 columns) or the `manual` test fails.
5. Add the checks in new files (`tests/case_tools2.py`, `tests/case_shell_conform.py`) with the idiom of `tests/test_shell.py`
   (`test_shell.run(vm, cmd)`); tests that wait for a shell prompt must boot with `cmdline="text"`.
6. Finally fix the vi differences listed in VI.md and re-run the differential test (`python tests/host/vi_diff.py gen|guest|sig|show|clean`,
   described in VI.md; it needs vim on the host and removes its temporary files with `clean`; never commit them).

## Tests run for this state

* Run on the merged main **before** the vi branch was merged (all `PASS`): `manual`, `pager`, `awk_checks`, `awk_big_input`,
  `diff_formats`, `diff_dirs`, `patch_checks`, `diff_big`, `bc_mathlib`, `dc_stack_machine`, `bc_dc_manual_pages`, `vi_manual`,
  `vi_basic`. `bc_language` failed 1 of 169 checks (the SIGPIPE log line above).
* Run on the helper branches before the pause (see their sections above).
* Run on the final head: **the build only**. Not run on the final head: every test (the vi tests, `shell`, `manual`, the suite).
