# vi: status and how to resume

`vi` is a modal vi/vim-lite editor, installed as `/bin/vi` (`vim` and `view` are kappabox applets in
`user/bin/kappabox/vilinks.c` that exec it; `view` starts read-only). Manual page: `share/patchouli/1/vi.pw`.
`pnote` stays the default `$EDITOR`.

## Source layout (`user/bin/vi/`)

| File | What is in it |
|---|---|
| `vi.h` | shared types and prototypes |
| `buf.c` | line array (lines borrow bytes from the loaded file until edited), undo/redo (line-range records, a step per command), registers, marks, load/save (temp file + rename, in place for symlinks/devices, no-final-newline and empty files kept) |
| `input.c` | key decoding (Esc timeout 120 ms, CSI keys), type-ahead stack, macros, "." recording, `:map`/`:imap` |
| `screen.c`, `pager.c` | painting (wrap/nowrap, numbers, list, status line, message row, search highlight), command-line editing with history, `-- More --` output |
| `motion.c` | all motions, text objects, regex translation (vim magic to libgap BRE), search |
| `normal.c` | normal/visual/insert/replace modes, operators, put, join, shift, reflow, Ctrl-A/X, scrolling |
| `ex.c` | ranges and the `:` commands |
| `main.c` | arguments (`-R -c +N +/pat`), start-up, main loop |

## State at the pause

Done and checked in the guest (`python tests/run_all.py vi_`): insert/append/open, x dd dw cw yy p, undo and redo, `/` search,
counts on inserts, `.`, D, J, saving with `:wq`; the manual page parses (`patchouli --check-all`), the `vim` and `view`
applets exist and `view` shows `[RO]`.

Written and built but NOT yet exercised by any test (expect bugs): every other motion and operator, text objects, numbered and
named registers, macros, visual modes (including block I/A), `:s` (all flags and `\1 \u \U ...`), `:g`/`:v`, ranges
(`/pat/`, `'a`, `;`), `:m :t :j :normal :sort :r :w !cmd :{range}!cmd`, `:set`, mappings, completion (Ctrl-N/P), `gq`,
long-line wrapping and scrolling, 100,000-line files, UTF-8 and invalid-byte round trips, tiny windows (1 and 2 rows),
signals, `-c`/`+N`/`+/pat`, `E37`/`E45` messages, exit status of `:cq`.

Not implemented: `U` (line undo; the key just beeps), `.` after visual-mode changes (deliberately not repeated), a real jump
list (only the last jump is kept), `:abbreviate` expansion in insert mode (the table exists, the hook is a stub), lines taller
than the window are only shown from their start, `:ene`, `\v \V \zs \%(` in patterns. `.` in a pattern matches one byte.

## Tests

`tests/case_vi.py` has `vi_manual` and `vi_basic` (10 checks). The plan for the other ~50 checks is in the task text:
groups of keys typed into `vi /tmp/x`, then `:wq`, then `cat` and compare; screen checks through `tests/vt.py`.
`edit(g, content, keys)` in the test file does one such group; pass `quit_keys=""` for tests that quit by themselves
(`ZZ`, `:x`, `:q!`).

Note for whoever merges: the tests in `tests/case_text.py` boot with `cmdline=""`, which in this tree starts the desktop
session and never prints the serial shell prompt, so `pager`, `manual`, `pnote_*` time out here (they pass with
`cmdline="text"`, which is what `case_vi.py` uses). The `manual` test therefore could not be run; `vi_manual` runs the same
`patchouli --check-all` and checks the vi page and the two applets.

## Resuming

1. `python tools/build.py && python tests/run_all.py vi_` to see the baseline.
2. Add check groups to `tests/case_vi.py` in batches (motions, operators, registers, dot, macros, text objects, visual,
   ex ranges, `:s`, `:g`, screen/scroll/long lines/UTF-8/startup options/exit statuses) and fix what fails.
3. Keep the manual page in step: it documents the full target feature list, so anything found missing should be implemented
   or removed from `vi.pw`.
