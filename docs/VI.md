# vi: status and how to resume

`vi` is a modal vi/vim-lite editor, installed as `/bin/vi` (`vim` and `view` are kappabox applets in
`user/bin/kappabox/vilinks.c` that exec it; `view` starts read-only). Manual page: `share/patchouli/1/vi.pw`.
`pnote` stays the default `$EDITOR`.

## Source layout (`user/bin/vi/`)

| File | What is in it |
|---|---|
| `vi.h` | shared types and prototypes |
| `buf.c` | the line array (a gap buffer; lines borrow bytes from the loaded file until edited), undo/redo (line-range records, a step per typed command, step numbers and times for the `u` message), `U`, registers, marks and the jump list, load/save (temp file + rename, in place for symlinks/devices, `vi -`, CRLF and missing-final-newline handling) |
| `input.c` | key decoding (CSI and SS3 keys one byte at a time, Esc timeout 120 ms, unknown sequences swallowed, Shift/Ctrl-arrows), the byte source stack (push-back, `-s` script, terminal), type-ahead, macros, "." recording (also visual changes), `:map`/`:imap`, `:abbreviate`, `intr_check()` (Ctrl-C during long work) |
| `screen.c`, `pager.c` | painting (wrap/nowrap, tall lines, numbers, list, status line and ruler, message row, search highlight), vim-style scrolling (`scroll_to_cursor` runs after every command), command-line editing with history, the hit-enter and `-- More --` pager |
| `motion.c` | motions, text objects, pattern translation (vim magic to libgap BRE), search with offsets |
| `normal.c` | normal/visual/insert/replace modes, operators, put, join, shift, reflow (`gq`), Ctrl-A/X, scrolling, insert-mode keys |
| `ex.c` | ranges and the `:` commands (`:s` across lines, `:g`, `:normal`, `:sort`, filters, options) |
| `main.c` | arguments (`-R -c -s +N +/pat -`), start-up, the main loop |

## State when paused (2026-10-23)

The work was stopped on request in the middle of a differential-testing round. The tree builds
(`python tools/build.py`).

### Done and checked in the guest

About 950 scripted checks (`vi -s keys file`, exact comparison of the saved file) plus typed-mode screen tests, all passing
at the last full run (19 tests: `vi_manual vi_basic vi_motion_{basic,words,search} vi_edit_{operators,registers_dot_macros,undo_insert}
vi_ex_{lines,subst_global,files_options} vi_textobjects vi_visual_modes vi_startup_and_files vi_screen_{basic,scroll,long_lines,keys_serial}
vi_exit_paths`):

* motions with counts, operators (`d c y > < ! = gu gU g~ g? gq gw J gJ`, doubled forms, forced `v V Ctrl-V`), text objects, registers
  (named, append, numbered shifting, `"-`, read-only ones), dot repeat (insert sessions, counts, visual changes), macros (counts,
  recursion, abort on error), marks, jump list (`''`, Ctrl-O, Ctrl-I, `:jumps`), undo/redo/`U` with Vim's cursor rules and messages,
  insert-mode keys (Ctrl-W U H T D R V O N P E Y A, autoindent, expandtab, softtabstop, textwidth), Ctrl-A/Ctrl-X, visual modes
  (`I A $A c r p J u U ~ :'<,'>`), abbreviations, ex ranges and commands (`:s` with all flags and `\1 \u \U \l \L \e \E \r \n`,
  `:g`/`:v`, `:m :t :j :> :< :normal :k :sort :r :w !cmd :{range}!cmd :w >> :e :n :set`), mappings, start-up options, odd files
  (empty, no final newline, CRLF, tabs, UTF-8, invalid bytes, read-only).
* the screen: status line and ruler (`All Top Bot NN%`), mode indicators, `:set number/relativenumber/list`, Ctrl-E/Y/D/U/F/B and
  `zt zz zb` with Vim's thresholds, wrapped and `nowrap` lines, lines taller than the window, `@` rows, error and info messages,
  `:reg :marks :jumps` in Vim's layout, the hit-enter and `-- More --` prompts, search echo on the command line.
* terminal handling: Esc and arrow keys one byte at a time (serial console), CSI and SS3 forms, Home/End/PgUp/PgDn/Delete/Insert,
  unknown sequences ignored; the terminal is restored after `:q :q! ZZ ZQ :x :wq :cq` and on SIGTERM/SIGINT/SIGHUP.
* performance on a 100,000-line file (guest, emulated CPU): `G`, `1G`, `500Gdd`, `100000dd`, `:%s/1/X/g`, `:g/9/d`, undo of those,
  a 2000-fold macro, 5000 `j` keys, a search, `:sort`, and edits on a 100,000-character line each take 0.3-2 s; `:g/5/m0` takes about
  60 s (every move shifts the gap buffer; `:g/^/m0` on 20,000 lines takes 6 s).
* found and fixed on the way: a heap overflow in `ln_splice` (the first edit of a never-edited line that shrinks it by more than
  about 32 bytes, e.g. `D` or `dw` on a long line, overflowed the new buffer; it crashed vi with a corrupted heap; the 100,000-character
  line rows of `vi_big_file` found it), and libc's backwards `memmove` (`std; rep movsb`) being far too slow for the gap buffer (now
  `lines_copy` in `buf.c`).

### Written after the last full test run, NOT run by any test

* `lines_copy`/`ln_splice` fix: exercised by about 4,800 differential cases and the `D`/`dw`/`cc` probes (all fine), but the
  regression groups were not re-run afterwards.
* `intr_check()` (Ctrl-C stops a macro that calls itself, `:g`, `:s`, `:normal` over a range) and its test `vi_screen_interrupt` in
  `tests/case_vi_screen.py`: never run. Expect to calibrate the test, not the editor, if it fails.
* `tests/case_vi_files.py` `vi_big_file`: rows for `D`, `cc` and `$x` on the 100,000-character line were added after the last run of
  that test (its other rows passed; the two failures of that run were wrong expectations, now corrected).
* `vi.pw` was edited (new options, `-s`, `-`, `U`, jump list, abbreviations, pager keys, notes) but `patchouli --check-all` has not
  been run on it: run `python tests/run_all.py vi_manual,manual` first.

### Half done: the differential test against a real vim

`tests/host/vi_diff.py` generates random but reproducible key sequences, runs them through the host's vim (9.2 from Git for Windows,
`-u NONE -N -n -X -s keys file`, UTF-8, 80x24) and through `vi -s` in the guest (one boot, a small C driver `tests/host/vifuzz.c`
that the tool copies to `user/bin/` for the run), and compares the saved files. First results (cases with the seeds in the tool's
docstring; `seq` and `ex` were generated but not run yet):

| family | cases | differing | hangs/crashes |
|---|---|---|---|
| motion (a motion, then `iX<Esc>` marks the cursor) | 800 | 47 | 0 |
| op (operator + motion, counts, `v`/`V` forcing) | 2500 | 266 | 0 |
| obj (text objects with operators and in visual mode) | 1500 | 97 | 0 |
| seq, ex | 2000, 1500 | not run | |

The differences found so far fall into these groups (Vim's rule in brackets; fix `motion.c`/`normal.c`, then re-run the same family
with the same seed and watch the number go down):

1. Counts that cannot be satisfied. `{ } [[ ]] [] ][` (Vim's `findpar`) and `( )` (`findsent`) fail as a whole, the cursor does not
   move and an operator is cancelled, unless the failing step is the last of the count and runs into the end or start of the file
   (then the cursor goes to the first or last line). `+ - <CR> _ $` and `j k` clamp instead (`cursor_down`/`cursor_up`: they fail
   only when the cursor is already on the last/first line and the count asks for a move). Here the code does the opposite: it
   clamps `{ } ( ) [[ ]]` and fails `+ - <CR> _ $`.
2. `]]`/`[[`/`][`/`[]` ending: for a plain motion the cursor goes to the first non-blank of the target line; with an operator `]]`
   to the last line puts it on the last character and makes the motion inclusive (Vim's `findpar` end code); `}` to the last line
   likewise but without the first-non-blank step. Sections also stop at a `.SH`-style nroff macro and at a form feed in column 1.
3. A search with a line offset (`/pat/+1`, `?pat?-2`) puts the cursor in column 1, not on the first non-blank.
4. `gj` and `gk` are not screen-line motions (Vim: charwise, along wrapped lines; with `nowrap` the same as `j`/`k`); `g0 g^ g$ gm gM`
   are missing.
5. `gq`/`gw` (`reflow_lines` in `normal.c`) rebuilds the paragraph from words and so collapses runs of blanks inside lines. Vim joins
   the lines of a paragraph the way `J` does (leading blanks of the next line removed, one blank or two after a period with
   'joinspaces') and breaks only at the last blank before 'textwidth' (79 when it is 0 and the window is wide enough: min(columns - 1,
   79)), dropping the blanks at the break; a line that already fits is left alone, and the indent of the first line of a paragraph
   is rewritten with tabs as 'noexpandtab' says. The continuation lines get no indent unless 'autoindent' is on.
6. Sentences and `is`/`as`, paragraphs `ip`/`ap`, quotes, `a(` `i[` `a{` and `it`/`at`: about 97 differing cases, not triaged yet
   (`python tests/host/vi_diff.py sig obj`). The `(`/`)` motions should be a port of Vim's `findsent`, then `is`/`as` of `current_sent`.
7. Others in the op list: `<BS>` as an operator motion, `cv` and `dv` with `}`/`G`/`gg`/`L`/`M`/`H` (forced charwise/linewise), `c)` and `y(`.

How to run it (needs a vim on the host; `$VIM` or Git for Windows' `usr/bin/vim.exe`):

    python tests/host/vi_diff.py gen op 2500 12      # cases + vim's answers -> initrd/fuzz/op.dat, user/bin/vifuzz.c (temporary)
    python tests/host/vi_diff.py guest op            # build, boot, run in the guest, report (about 4 minutes for 2500 cases)
    python tests/host/vi_diff.py sig op              # the differing cases grouped by their keys
    python tests/host/vi_diff.py show op 97 391      # input, keys and a diff of vim's and vi's result
    python tests/host/vi_diff.py clean               # remove the temporary files again; never commit them

Things learned about the oracle: vim must run with `--cmd 'set encoding=utf-8'` (otherwise it treats the text as latin-1); `-es` mode
starts on the last line and `:normal` aborts the rest of its keys on an error (so it is no substitute for `-s`); a `-s` script has to
end with `:wq<CR>` (a final `ZZ` is not run before vim sees the end of the input); do not put `Esc` directly in front of `O` or `[`
(a terminal could read that as one key; the generator drops such cases); `nrformats` is set to `hex` because vi's Ctrl-A knows
decimal and `0x` only.

### Not started

`\v \V \zs \%(` in patterns, search patterns that span lines (`/foo\nbar`; `:s` already handles `\n`), non-greedy repeats (libgap's
regex lacks them), `vi -` with a terminal on standard input, `:changes`/`:undolist`, the `"=` register, Ctrl-K digraphs, command-line
completion, `:earlier`/`:later`/`:undo N`. A faster `:m` (a rotation instead of a delete and an insert) would halve `:g/^/m0`.

## Tests

* `python tests/run_all.py vi_` runs everything (the filter is a prefix; `vi_big` alone takes about 90 s, `vi_ex` also matches
  `vi_exit_paths`). Never two runs at once (they share `build/esp`), and not the whole suite while a different agent is booting.
* `tests/vilib.py`: `Row`/`R()`/`C()` rows run through `ScriptRunner` (`vi -s`; the whole table in one boot, about 0.4 s a row) and the
  typed-mode `Guest` (keys over the serial line, the screen read back through `tests/vt.py` with `LINES=16 COLUMNS=70`).
  The shell function there works around gapsh: `"$@"` with no arguments is one empty word (so the file name is always passed),
  `printf` needs `\0NNN` for bytes (a leading 0), and there is no `timeout` or `stty`.
* `case_vi.py` (manual page, applets, basic), `case_vi_motion.py`, `case_vi_edit.py`, `case_vi_ex.py`, `case_vi_visual.py`,
  `case_vi_files.py` (start-up, odd files, `vi -`, the 100,000-line file), `case_vi_screen.py` (typed-mode screens, serial keys,
  exit paths, Ctrl-C).
* Wait for screen output, never sleep for a fixed time; compare files exactly.

## Resuming

1. `python tools/build.py`, then `python tests/run_all.py vi_manual,manual` (the page was edited blind), then
   `vi_big,vi_screen_interrupt`, then `vi_`, `shell` and `manual`. Fix what the unrun items above turn up.
2. Work through the differential groups in "Half done", one family at a time, with `vi_diff.py`; after each fix re-run that family
   with the same seed, then the matching `vi_` tests, then commit.
3. Run the `seq` and `ex` families (they exercise undo, registers, macros, `:g`, `:s`) and triage.
4. Keep `vi.pw` in step with the editor (no `U`/jump-list/abbreviation claims were left in it; the notes section lists the known gaps).
