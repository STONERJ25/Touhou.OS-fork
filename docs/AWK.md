# awk

`user/bin/kappabox/awk.c` is a kappabox applet: POSIX awk with a few gawk extras (`systime`, `strftime`, `nextfile`, `**`,
regular expression `RS`). Manual page: `initrd/ScarletDevilMansion/share/patchouli/1/awk.pw`. Tests: `tests/case_awk.py`
(`python tests/run_all.py awk`: `awk_checks` has about 125 one-line checks, `awk_big_input` pushes 300,000 lines through
sums, fields and counting arrays).

## Design in short

- Lexer -> recursive-descent parser -> AST -> tree-walking interpreter. A `/` in operand position is rescanned as a regex.
- Values are `{type, number, pointer, length}`; temporaries live in an arena that is reset after every statement, variables
  and array elements own their strings. Types: number, string, "strnum" (numeric string) and "input" (undecided, checked
  when compared).
- Arrays are chained hash tables that keep insertion order (so `for (k in a)` runs in insertion order).
- Records are read with 64 KB `read()` buffers; fields are split lazily and point into the record; `$0` is rebuilt lazily
  after a field or `NF` assignment. Output is buffered by hand (flushed before `system()`, before a pipe is opened, at exit,
  and per statement when stdout is a terminal).
- Regular expressions go through libgap's `regcomp` (REG_EXTENDED) after the awk escapes and bracket expressions are
  converted; plain strings without metacharacters use `memmem`. Dynamic regexes are cached by source string.
- `exit`/`next` inside a function leave through `longjmp` (`g_jmp`), everything else uses return codes.

## State

Done: everything in the task list: patterns (BEGIN/END/range/regex/expr), fields and all special variables, strnum
semantics, `getline` in all six forms, `print`/`printf` with `>`, `>>` and `|`, `close`/`fflush`/`system`, user functions
(recursion, arrays by reference, locals), `-F`/`-v`/`-f`/`--`/`--version`, command line `var=value`, error exit status 2.

Known limits / not done:

- Regex matching is leftmost-first (libgap), not POSIX leftmost-longest: `match("ab", /a|ab/)` gives length 1.
- Strings are counted in bytes (`length`, `substr`, `index`, printf width); UTF-8 is only respected by `%c` and `%s` width.
- No `gensub`, `asort`, `toupper` on UTF-8, `printf %a`, `IGNORECASE`, `RT`, `FPAT`, `BEGINFILE`.
- `getline < file` keeps the file open until `close()` or exit; a file and a command with the same name share nothing.
- Deep recursion is limited by the 8 MiB stack (about 5,000 levels of a simple recursive function work; deeper stops with
  "function call nesting too deep").
- Not yet checked on the framebuffer console; the tests use the serial text console.
- `python tests/run_all.py manual` timed out in this worktree while booting with an empty command line (the guest never
  printed a prompt within 90 s on the busy host); the awk page itself is validated by the `patchouli --check-all` check
  in `awk_checks`.

How to resume: read the header of `awk.c` (sections are marked by `/* ---- ... */` banners), add a check to `TABLE` in
`tests/case_awk.py` for any new behaviour, run `python tools/build.py && python tests/run_all.py awk`.
