# Text tools: Patchouli's library

The interactive text tools of the system, and the manual pages that go with them. Everything here is userland: it uses
only `libgap` and the console as it is (an ANSI terminal on the serial line or on the framebuffer, see `keys(7)`).

| Program | Also known as | Source | What it is |
|---|---|---|---|
| `pnote` | `$EDITOR` | `user/bin/pnote/` | full-screen editor, nano-like ("Patchouli's Notebook") |
| `patchouli-pager` | `less`, `more` | `user/bin/patchouli-pager/` | pager |
| `patchouli` | `man` | `user/bin/patchouli/` | manual viewer for the `.pw` pages |
| `omikuji`, `fortune`, `saisen`, `reimusay`, `kappasay` | | `user/bin/kappabox/gensokyo.c` | shrine toys |
| (header-only) | | `user/bin/ptlib/` | UTF-8 and terminal helpers shared by the three programs above |

`less`, `more` and `man` are kappabox applets (`user/bin/kappabox/library.c`) that exec the real program under that name;
`pager.c` looks at `argv[0]` to switch to `more` behaviour.

## pnote

```
pnote [options] [+LINE[,COL]] [FILE]      pnote -  edits standard input
  -n  line numbers     -S  no highlighting    -I  no auto-indent    -E  Tab inserts spaces
  -T N  tab width      -R  view only          -l c|sh|md|none  force a highlighter
```

The screen has a shrine-red title bar, the text, a status line and two rows of key hints (`^G` opens the full list; the
same list is the `pnote(1)` page). Keys follow nano: `^S` save, `^X` exit (a question if there are unsaved changes), `^W`
search, `^N`/`^P` next/previous match, `^R` search and replace (Y/N/A), `^K` cut a line, `^U` paste, `^Z`/`^Y` undo/redo,
`^L` go to a line, `^A`/`^E` start/end of line, `Alt-A` selection, `^T` toggles (line numbers, highlighting, auto-indent,
tabs as spaces, CRLF/LF), `^C` position. In the search prompt `Alt-C` switches case-sensitivity and `Alt-R` switches to
extended regular expressions (then `\1`-`\9` and `&` work in the replacement). Searches are smartcase and work per line.

Behaviour worth knowing:

- Files without a final newline stay that way; CRLF files keep their CR LF (shown as `[CRLF]` in the title bar); an empty or
  missing file is fine ("New file"); a read-only file opens with a warning and a save offers another name.
- Text is UTF-8: the cursor moves by code point and a column is one code point (wide CJK characters count as one
  column, like on the framebuffer console). Invalid bytes are kept as they are.
- A long line scrolls sideways; `<` and `>` mark text hidden to the left or right.
- Undo is many levels deep; a run of typing, a paste or a replace-all is one step.
- Lines are held in a gap array and borrow their bytes from the loaded file until they are edited, so a 100,000-line file
  opens at once and scrolling repaints only the rows that changed.
- `pnote -` reads standard input. If standard output is also not a terminal, leaving the editor writes the text to it
  (like `vipe`): `cmd | pnote - | cmd2`. The keys come from `/dev/tty`.
- The size comes from `TIOCGWINSZ` and is polled, so a resized console is picked up; `LINES` and `COLUMNS` in the
  environment override it (the tests use `LINES=16 COLUMNS=70`).
- Highlighting is modest on purpose: C (`.c .h`), shell (`.sh`, `.gaprc`, a `#!` line) and Markdown (`.md`), by file name or `-l`.

## patchouli-pager (less, more)

```
patchouli-pager [-NSRFIe] [+LINE | +/PATTERN | -p PATTERN] [--title TEXT] [FILE ...]      command | patchouli-pager
```

`SPACE`/`b`/`f`/`PgDn`/`PgUp` page, `j`/`k`/arrows/`Enter` scroll, `d`/`u` half a page, `g`/`G`/`Home`/`End` ends (with a
count: `50g` is line 50), `50p` a percentage, `/` and `?` search (extended regular expressions, smartcase), `n`/`N` repeat,
`-N` and `-S` toggle line numbers and chopping (then the arrows scroll sideways), `:n`/`:p` switch files, `h` help, `q` quit.
`-R` passes ANSI colours through, `-F` quits at once if the text fits one screen, `-e` quits at the end. `LESS` can hold
default options. Text is read lazily, so `yes | patchouli-pager` shows the first screen at once; when the text comes from
a pipe the keys come from `/dev/tty`. If standard output is not a terminal the pager behaves like `cat` (with `-N` it numbers
the lines). SIGINT, SIGTERM, SIGHUP and SIGQUIT give the terminal back before the program exits. As `more` the status
line reads `--More--(NN%)` and the program leaves when it runs off the end.

## patchouli (man)

```
patchouli [SECTION] NAME      patchouli -k WORD      patchouli -f NAME      patchouli -l      patchouli --check-all
```

Pages live in `initrd/ScarletDevilMansion/share/patchouli/<section>/<name>.pw` (installed as
`/ScarletDevilMansion/share/patchouli/...`; `PATCHOULI_PATH` can point elsewhere). Sections: 1 commands, 5 file formats,
7 overviews, 8 administration. On a terminal the page is rendered with bold and colour and handed to `$PAGER`; into a
pipe it is plain text. `--check-all` renders every page and reports problems (it is part of the test suite); `patchouli
FILE.pw` renders a file you are writing.

There is a page for every kappabox applet, every gapsh builtin (`gapsh-builtins(7)` plus one page each for the common ones),
the shell language (`gapsh-language(7)`), globbing, regular expressions (`regex(7)`), the keyboard (`keys(7)`) and the
layout of the system (`gensokyo(7)`). The test `manual` fails if an applet has no page, so **a new applet needs a page in
`share/patchouli/1/`**.

### The .pw format

A page is plain UTF-8 text. The full description is `patchouli pw` (section 5); in short:

```
.page frob 1 turn the frob          header: NAME SECTION one-line description (no full stop)
.alias unfrob                       other names for the page
# SYNOPSIS                          section headings are capitals; commands need SYNOPSIS and DESCRIPTION
*frob* [*-v*] _FILE_                SYNOPSIS lines stay separate lines
# DESCRIPTION
Text lines are joined and wrapped.  *bold*  _underline_  `literal $text`  \* (escape)
    lines indented by 4+ are verbatim (at most 78 columns)
- a bullet
@ *-v*                              a definition item: the term, then the text indented by 4
    Say how far.
; a comment
# SEE ALSO
cat(1), regex(7)                    every NAME(N) here must be a real page (checked)
```

`*` and `_` open a span only at the start of a word and close it at the end of one, so `snake_case` and `2 * 3` need no
escaping. Tabs are rejected. Keep OPTIONS to what the program really implements.

## The shrine

- `omikuji` draws today's slip: a rank (Dai-kichi, Chu-kichi, Sho-kichi, Kichi, Sue-kichi, Kyo) and a line each for wish,
  travel, business, health, lost items and love. The slip is seeded by user id and UTC date, so it is the same all day;
  `-r` draws a random one, `fortune` is `omikuji -r`. Boxed and coloured on a terminal, plain text in a pipe. The kanji
  (`-k`) are off by default because the console font has none; `-a` avoids box-drawing characters.
- `saisen [AMOUNT]` puts yen (default 5) into the offering box. The total is kept in `/var/lib/saisen/total` if that can be
  written, else `/tmp/saisen-total`; `-s` only shows it. Reimu reacts to the amount, including 0.
- `reimusay` and `kappasay` draw a speech bubble (`-t` thought bubble, `-n` no wrapping, `-W N` width) with text from the
  arguments or standard input, wrapped to 40 columns by default.

## ptlib

`user/bin/ptlib/pt_utf8.h` has the UTF-8 helpers (decode/encode, next/previous code point, word characters; bytes that are
not valid UTF-8 become the code points `0xDC80+byte`, so they survive a round trip). `pt_term.h` has the terminal layer:
raw mode with restore on exit and on signals, key decoding (`PT_K_*`, `PT_M_ALT`...; an ESC waits 250 ms for the rest of
a sequence), the window size, and a diff-based screen painter that rewrites only the rows that changed. A directory
under `user/bin` with no `.c` files is skipped by `tools/build.py`, so `ptlib` produces no binary of its own.

## Tests

```
python tests/run_all.py pnote     # pnote_edit, pnote_commands, pnote_big
python tests/run_all.py pager     # also: manual, shrine; with no argument everything runs
```

`tests/case_text.py` boots the guest, types control bytes and escape sequences into the console exactly as the keyboard
produces them, and checks two things: the screen, by replaying the serial output through `tests/vt.py` (a small
terminal emulator with `rows_text()` and `row_attrs()`), and the result, by `cat`-ing the saved files. Full-screen
programs are started with `LINES=16 COLUMNS=70`. Waiting is always for something to appear on the screen, never a fixed
delay, because the guest can be slow when the host is busy (a guest `sleep 3` has taken tens of seconds of real time).

## Known limits

- Search and replace work within a line (no patterns across lines). The regex engine is libgap's: byte-based, backtracking.
- Wide (CJK) and combining characters are treated as one column each.
- No mouse, no split windows, no multiple buffers in `pnote`; `pnote` has no word wrap.
- The pager reads a whole pipe into memory (lazily); it does not follow growing files (`F` in less).
