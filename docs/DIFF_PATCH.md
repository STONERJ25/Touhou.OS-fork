# diff and patch (kappabox applets)

Status when work was paused (branch `worktree-agent-ac04a73bcf43119ad`). Everything listed under Done is committed, builds
with `python tools/build.py` and passes `python tests/run_all.py diff_` and `python tests/run_all.py patch_checks`.

## Done

- `user/bin/kappabox/diff.c`: normal, `-u`/`-U N`/`--unified[=N]`, `-c`/`-C N`/`--context[=N]`, `--normal`, `-q`, `-s`, `-r`, `-N`,
  `-a`, `-b`, `-w`, `-i`, `-B`, `-d`, `-x PAT`, `--label` (twice), `--strip-trailing-cr`; directories (`Only in`, `Common
  subdirectories`, `diff -r a/x b/x` header lines with the options as typed), a file against a directory, `-` for stdin,
  binary files, `\ No newline at end of file`, hunk merging, exit status 0/1/2. Output was checked against GNU diff 3.12
  (the Git Bash `diff` on the dev machine) for every case in `EXACT` in `tests/case_diff.py`.
  Algorithm = GNU's pipeline: common head/tail stripped, lines without a partner discarded (`discard_confusing_lines`), Myers
  with the linear-space middle snake and cost limit (`too_expensive`, floor `MIN_COST_LIMIT` = 2048; the big-snake heuristic
  switches on above 100000 lines), `shift_boundaries`. 150000-line files with 3 changes take a few seconds in the guest.
- `user/bin/kappabox/patch.c`: unified, context and normal diffs; `-p N -d DIR -i FILE -o FILE -R -E -b -z SUF -r FILE -s
  --dry-run`; exact matching at the stated line, then nearest offset (`Hunk #N succeeded at L (offset K lines).`), rejects to
  `NAME.rej` (unified), file creation/deletion (`/dev/null`), several files per patch, `Index:` and `diff ...` lines skipped,
  the file chosen like GNU (existing one, new name first), no-newline marker, CRLF kept.
- Manual pages `diff.pw`, `patch.pw`. Tests: `tests/case_diff.py` (`diff_formats`, `diff_dirs`, `patch_checks`,
  `diff_roundtrip`, `diff_big`).

## Half done / not done

- `patch -N` / `--forward` is accepted but does nothing: a reversed or already-applied patch just fails its hunks into `.rej`.
  GNU says `Reversed (or previously applied) patch detected!  Assume -R? [n]` (or `Skipping patch.` with `-N`). To add it: in
  `apply_one`, when the first hunk finds no place, search for its reversed text; if found, print the message, treat the
  file as skipped (`N out of N hunks ignored`), exit 1.
- `patch` has no fuzz and none of GNU's rule that a hunk with less leading than trailing context must match at the start of
  the file; the reject file is always unified and its header has the names without dates; the "File to patch:" question is
  never asked (it prints `can't find file to patch at input line N` and skips). Not implemented: `-l`, `-D`, `-B`, `--merge`,
  git rename/mode/binary headers, `-V`.
- `diff`: not implemented `-e` (ed script), `-y`, `-p`/`-F` (function names), `-I RE`, `-Z`, `--from-file`/`--to-file`, `--color`;
  `-T`, `-t`, `-H` are accepted and ignored. Header times show `.000000000` because the kernel's stat has whole seconds only.
  `-B` with `-b`/`-w` follows the GNU rule but only the plain `-B` case is tested.
- `python tests/run_all.py manual` did not get as far as reading a page on the dev machine: the guest booted with an empty
  command line never printed the shell prompt within 90 s (before any of this code runs). Checked instead in a text-mode
  guest: `patchouli --check-all` = 150 pages, 0 problems, and every applet in `kappabox --list` has an index entry.

## Resuming

```
python tools/build.py
python tests/run_all.py diff_       # diff_formats diff_dirs diff_roundtrip diff_big
python tests/run_all.py patch_checks
```

On a machine with GNU diff/patch on the host, new expectations for `EXACT` can be taken from the real tool (run the same
command in a directory holding the `FILES` of the test). `diff_roundtrip` takes about 100 s; lower the case count in it if
that is too slow.
