# Utilities: what exists, what does not

This page tracks the work "close the gap between this userland and a comfortable Unix command line". It says plainly what is
done, what is only partly done and what was never started. Details of the bigger tools live in their own pages:
[AWK.md](AWK.md), [DIFF_PATCH.md](DIFF_PATCH.md), [BC_DC.md](BC_DC.md), [VI.md](VI.md). The manual pages are in
`initrd/ScarletDevilMansion/share/patchouli/1/` (see [TOOLS.md](TOOLS.md)); every tool listed as done has one.

## State when paused

The work was paused early (the session was cut short). Four big tools were written by parallel helper agents, each on its
own branch, and merged here; everything else on the task list was **not started**.

### Done (built, committed, tested in the guest as far as noted)

| Tool | Where | State |
|------|-------|-------|
| `awk` | `user/bin/kappabox/awk.c` | Complete POSIX awk (about 3,200 lines): all pattern and statement forms, fields, `FS`/`RS` incl. paragraph mode and regex `RS`, arrays, all `getline` forms, redirections and pipes, user functions, every POSIX builtin plus `systime`/`strftime`. Tests `awk_checks` (about 125 checks) and `awk_big_input` (300,000 lines) passed on the commit before the last two small edits; the last edit fixed one test line and was not re-run. Limits: leftmost-first regex matching (libgap's engine), byte-counted strings, no `gensub`/`asort`/`IGNORECASE`. |
| `diff` | `user/bin/kappabox/diff.c` | Myers diff with the GNU pipeline (common head/tail stripped, unmatched lines discarded, linear-space middle snake with cost limit): normal, `-u`, `-c`, `-q`, `-s`, `-r`, `-N`, `-a`, `-b -w -i -B`, `-x`, `--label`, `--strip-trailing-cr`, directories, binary detection, exit 0/1/2. Output compared with GNU diff 3.12. No `-e -y -p -I --color`. Header times end in `.000000000` (the kernel's `stat` has whole seconds only). |
| `patch` | `user/bin/kappabox/patch.c` | Unified, context and normal diffs, several files per patch, `-p -d -i -o -R -E -b -s --dry-run`, exact matching with offset search, `.rej` files, create/delete via `/dev/null`. **Not done:** `-N`/reversed-patch detection (accepted, does nothing), fuzz (deliberately none). Tests `diff_formats diff_dirs patch_checks diff_roundtrip diff_big` passed. |
| `bc` | `user/bin/kappabox/bc.c`, `bignum.c` | Full bc language, `-l` math library (`s c a l e j`), GNU line wrapping, errors; arbitrary precision (base 10^9 limbs, no Karatsuba). 93 of 94 checks in the last guest session passed (the failure was a manual-page line that was shortened afterwards, not re-run); the 169-check `bc_language` table passed apart from the banner checks, which changed afterwards and were not re-run. |
| `dc` | `user/bin/kappabox/dc.c` | All standard commands, registers and register arrays, macros with tail calls, GNU error messages and wrapping. 71 checks passed. |
| `vi`, `vim`, `view` | `user/bin/vi/` (about 5,800 lines), `vilinks.c` | A first version of the modal editor. **Only 11 checks have ever run in the guest** (start-up, insert/append/open, `x dd dw cw yy p D J`, undo/redo, `/` search, counts, `.`, `:wq`, the manual page). Everything else in the source (motions, text objects, registers, macros, visual modes, ex ranges, `:s`, `:g`, filters, mappings, `:sort`, 100k-line files) was written and compiles but has **never been run**: expect bugs. See [VI.md](VI.md). `$EDITOR` stays `pnote`; `vi` is for people who want vi. |

### Not started

Nothing below has been written; the existing versions of the existing tools are unchanged.

* **New tools:** `join`, `split`, `csplit`, `fmt`, `column`, `unexpand`, `tsort`, `timeout`, `time`, `watch`, `nohup`, `install`,
  `killall`, `pgrep`, `nice`/`renice`, `vmstat`, `fuser`, `getopt`, `du`, `sha1sum`, a standalone `printf`, `test`/`[` and `pwd`
  applets.
* **Weak existing tools to extend** (not touched): `factor` still does trial division, so a large 64-bit prime takes minutes
  (needs Miller-Rabin and Pollard rho); `shuf`, `seq`, `mktemp`, `realpath`, `strings`, `hexdump` (only `-C`), `xxd -r`, `fold`, `nl`,
  `expand`, `sort` (`-k` ignores character offsets, no `-h -V -M -m`), `uniq`, `cut`, `tr`, `paste`, `wc`, `xargs` (`-P` not
  needed), `ls`, `find`, `grep`, `sed`, `stat -c`, `date -d`, `cp/mv/rm/chmod/chown` option sets, `md5sum`/`sha256sum`.
* **gapsh audit** (nothing changed). Gaps found while reading the sources (`user/bin/gapsh/`):
  `<<-` here-documents are not parsed (the lexer makes `-EOF` the delimiter); `set -e -u -x -f -o pipefail` are accepted and
  ignored; `trap` handles only `EXIT` and the `''`/default signal dispositions (no handler strings for signals); `wait` takes one
  pid and no job specs; `exec` with only redirections does nothing lasting; no `printf -v`; `read` lacks `-s -n -d -t`;
  `test` lacks `-ef` and the usual `-a`/`-o` edge rules; arithmetic lacks `,` and `**` and some assignment operators;
  a script read from a pipe is joined with an O(n^2) loop (`main.c`); aliases and functions work.
* **Job control needs the kernel.** `SIGTSTP`/`SIGSTOP` are "default ignored", there is no stopped process state and no
  `WUNTRACED` (`kernel/sys/signal.c`, `process.c`), so Ctrl-Z cannot work and `fg`/`bg` only wait. A kernel change is needed
  first (a stopped state, `SIGCONT`, `waitpid(WUNTRACED)`), then gapsh. Ctrl-C reaching the foreground group was not checked.
* **Missing kernel/libc pieces** that tools would use: no `utimes` (so `touch` cannot set times), no hard links, no priorities
  (`nice`), no `/proc/<pid>/fd` (`fuser`). libgap: `fwrite`/`fputs` go byte by byte (slow bulk output), `strtod` is inexact
  (`"0.3"` parses as 0.30000000000000004), `printf %f` rounds approximately, and the regex engine is a recursive backtracker with
  leftmost-first semantics. Fixing `strtod`, `printf` and the regex engine would help every tool.
* **The shell-script conformance test** and `tests/case_tools2.py` do not exist yet.

### Known problems

* `python tests/run_all.py manual` (and `pager`) boot with `cmdline=""`, which starts the desktop and never shows a serial shell
  prompt, so they time out here after 90 s. The tests of the new tools use `cmdline="text"` and the equivalent
  `patchouli --check-all` passes in a text-mode guest (150 pages, 0 problems, per the diff/patch run). The fix is probably to boot
  those two tests with `cmdline="text"`; it was not tried.
* The full suite (`python tests/run_all.py`) was **not run** on the merged tree.

### How to resume

1. `python tools/build.py` (about 6 s).
2. Run the tests of the new tools, one at a time (they share `build/esp`): `python tests/run_all.py awk_`, `diff_`, `patch_`,
   `bc_`, `dc_`, `vi_` (about 4 minutes for the bc/dc group; `diff_roundtrip` alone takes 110 s).
3. For vi, add the remaining check groups to `tests/case_vi.py` (helper `edit(g, content, keys)`) and fix what fails.
4. Then work through "Not started" above in this order: libc fixes (`strtod`, `printf`, stdio bulk paths, regex), the gapsh gaps
   and a shell conformance test, the kernel stop/continue support and job control, the small new tools, the extensions of the
   existing tools. Each new applet needs a manual page (`share/patchouli/1/NAME.pw`) or the `manual` test fails.
5. Add your own checks in a new `tests/case_tools2.py` using the idiom of `tests/test_shell.py` (`test_shell.run(vm, cmd)`).

## Tests run and not run for this merge

Run on the individual branches before the merge: the tests named above (see the per-tool pages). Run on the merged tree:
the build only (`python tools/build.py` passes). Not run: the complete suite, `manual`, `pager`, the shrine tests.
