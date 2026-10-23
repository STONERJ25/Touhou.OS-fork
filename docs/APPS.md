# Desktop applications

Five applications that ship with HakureiDE, each in `user/bin/<command>/` and started from the Gensokyo menu (the menu lists
the commands in `user/bin/hakurei-shell/launch.c` whose `/bin/<name>` exists). They are written against `kappa.h`; see
`DESKTOP.md` for the toolkit and `THEMING.md` for the names. Window titles say what the window is; the Touhou names live in
the menu, the about boxes and the headings.

Every program has a text mode that prints what the window would show, used by `tests/case_apps.py`, and `--trace`, which
prints one line on stdout for each thing that happens (key, click, page, signal sent). Started from the panel, stdout goes to
`/tmp/<name>.log`.

## Calculator (Ran's Calculator): `calculator`

Standard and scientific keypads (View menu, Alt+1 / Alt+2, or the STD / SCI tags in the display), an editable expression line
with a live preview, a history tape on the right and memory keys. Numbers are x87 extended precision and shown with at most
15 significant digits ("0.3", "1.5e-7", "1.26765060022823e30").

* Grammar: `+ - * /`, implicit multiplication (`2pi`, `3(4+5)`), `mod`, `^` (right associative, `-2^2 = -4`, `2^-2` works),
  `!`, `%`, unary minus, parentheses, `x²`. A percentage after `+` or `-` is a share of the left side: `200+10%` is 220.
* Functions: `sqrt cbrt sin cos tan asin acos atan sinh cosh tanh ln log log2 exp pow abs floor ceil round fact min max mod root deg rad atan2`;
  constants `pi e tau phi ans`. Trigonometry works in degrees or radians (DEG / RAD tag, F3 / F4); exact angles give exact
  answers (`sin(180) = 0`, `tan(90)` is an error).
* Errors are explained in words and the offending piece is underlined in the expression line.
* Keys: digits and operators type into the line; Enter or `=` evaluates; Backspace, Delete, arrows, Home, End edit; Esc clears;
  Up / Down recall earlier expressions; Ctrl+C copies the result, Ctrl+V pastes an expression (spaces, thousands separators and
  the unicode `x`, `/`, minus and pi are cleaned up); Ctrl+L / R / M / P (Shift+P) are MC / MR / MS / M+ (M-); Ctrl+Q quits.
  Clicking a tape entry reuses its expression, right-clicking it inserts its result.
* `calculator --eval EXPR...` prints one result per argument (`--rad` / `--deg` choose the angle unit, `ans` chains), exit
  status 1 if any failed. The about box (Help menu) holds the only "Ran says the answer is ..." line.
* Reads: nothing. Writes: nothing.

## Pocket Watch (Sakuya's Pocket Watch): `pocketwatch`

Tabs: **Clock** (a procedurally drawn pocket watch with roman numerals, digital time, date and week number; everything is UTC
and says so), **Stopwatch** (centiseconds from `CLOCK_MONOTONIC`, laps with best / worst marked), **Timer** (presets, custom
H:M:S, remaining time in the window title, the window flashes, rises to the front and writes the bell character `\a` to
stdout every second until dismissed) and **Calendar** (month grid, ISO week numbers, today highlighted, year and month
navigation). Press **T** to let Sakuya stop time: the face freezes under a ring of knives until you press T again.

* Keys: Ctrl+1..4 tabs; stopwatch Space / L / R; timer Space pauses, Esc cancels, any of Space / Enter / Esc dismisses the
  alarm; calendar arrows (day / week), PageUp / PageDown (month), Shift or Ctrl with them (year), Home (today).
* Options: `--tab clock|stopwatch|timer|calendar`, `--timer SECONDS` (start a countdown at once), `--trace`;
  `pocketwatch --calendar [YEAR MONTH]` and `pocketwatch --date YYYY-MM-DD` print calendar facts and exit.
* Reads `~/.config/hakurei.conf` (`clock24`, re-read every few seconds) for the 12 / 24 hour display.

## Spell Manager (Task Manager): `spellmgr`, Ctrl+Alt+Del

Window title "Task Manager". **Spells**: a table of processes (PID, name, owner, state, CPU %, memory) from
`/proc/<pid>/stat`, sortable by clicking a header, a filter box (name, owner, PID, state), refreshed every second without
losing the selection or scroll position, **End Spell** (SIGTERM) and **Exorcise** (SIGKILL) with a confirmation dialog, and a
details pane (command line, parent, owner, start time, CPU time, VM / RSS, working folder). **Performance**: CPU and memory over
the last 60 seconds (CPU from the idle task's ticks in `/proc/tasks` against wall time), uptime, load average (estimated from
CPU use while the kernel reports 0.00), kernel heap and tmpfs. **Hardware**: processor, memory, display, PCI devices,
block devices, USB devices, mounts. **Users**: accounts and groups with a count of running spells.

* Keys: Up / Down / PageUp / PageDown / Home / End move the selection, Delete asks to end it (Shift+Delete: exorcise), Ctrl+F
  focuses the filter, Esc clears it, F5 refreshes, Ctrl+1..4 tabs. A second `spellmgr` raises the window already open
  (`--new` opens another).
* Processes that vanish while being read are skipped; an ended selection stays visible, marked as ended.
* `spellmgr --dump | --hardware | --users | --perf` print the same data as text.
* Reads: `/proc/<pid>/{stat,cmdline,status}`, `/proc/{tasks,meminfo,uptime,loadavg,cpuinfo,pci,partitions,usb,mounts}`,
  `/etc/passwd`, `/etc/group`, `/dev/fb0` (query only).

## Control Panel (Kappa Control Panel): `kappa-panel`

Window title "Control Panel". A sidebar of panes (Alt+1..7, Ctrl+1..7 or Up / Down):
**System** (kernel, computer name, uptime, processor, memory, signed-in user, copy a summary), **Display** (screen size and
work area from the window server, pixel format and pitch from `FBIOGET_INFO`, how to change the resolution), **Keyboard &
Mouse** (a picture of the keyboard that lights up pressed keys, the last key with its code and modifiers, and a mouse test
area with button, wheel and double-click counters; this pane takes every key so you can test them), **Devices** (disks, PCI,
USB), **Users** (accounts, groups, and **Change password...** which opens a terminal that runs `passwd` when `/bin/passwd`
exists; `KAPPA_PASSWD` can point it elsewhere), **Power** (Shut down and Restart, each after a confirmation, with a `sync`
first; `--dry-run` only prints what would happen) and **Appearance** (the theme palette, and the 12 / 24 hour clock).
* `kappa-panel --pane NAME`, `--trace`, `--devices`, `--conf get KEY`, `--conf set KEY VALUE`.

### `~/.config/hakurei.conf`

Plain text, one `key=value` per line, `#` starts a comment, unknown keys are kept when the file is rewritten (the panel writes a
temporary file and renames it). Known keys:

| key | values | meaning |
|-----|--------|---------|
| `clock24` | `1` (default) or `0` / `no` / `false` / `off` | 24 hour clock; `0` shows 12 hour time with AM / PM |

The Pocket Watch follows `clock24` live. The desktop panel (`hakurei-shell`) does not read the file yet; it only has to call a
reader like `conf_get()` in `user/bin/kappa-panel/conf.c` (about 30 lines) to adopt it.

## Bunbunmaru News: `bunbunmaru`

A gossip paper in five pages (buttons or keys: Left / Right, Home / End, 1..5; Up / Down / PageUp / PageDown / Space or the wheel
scroll a long page). Typeset into balanced columns with justified text, a drop cap, rules and boxes; the number of columns
follows the window width.

1. **Front page**: masthead, date line, the lead system story, two gossip stories, the weather box.
2. **The Shrine Gazette**: system news written from the real state of the machine when the app starts: uptime ("Local Shrine
   Runs For 3 Hours Without Incident"), memory (Yuyuko's larder), the busiest spells and zombies, mounted volumes, the PCI
   quarter, disks, Eiki's docket (the `[Eiki]` incident lines of `/dev/kmsg`) and notable kernel log lines in the paper's voice.
3. **Around Gensokyo**: gossip chosen by the date from a pool of 49 original stories, plus overheard remarks.
4. **Features**: the spell card of the day with a drawing of its bullet pattern, a horoscope for twelve signs, and a crossword
   generated from the date (click a square and type, Tab changes direction, Check / Reveal / Clear).
5. **Classifieds**: the day's notices in dashed boxes, letters to the editor and Kappa's Corner (a tip about the desktop).

* `bunbunmaru --print [--date YYYY-MM-DD]` prints the whole issue as text, `--pool` counts what the paper can draw on,
  `--page N` opens on a page, `--trace` logs page turns and puzzle moves. The same date always gives the same gossip.
* Reads: `/proc/{uptime,meminfo,mounts,pci,partitions,usb}`, `/proc/<pid>/stat`, `/dev/kmsg`, `/etc/passwd`. All text is original.

## Gensokyo Explorer (file manager): `explorer`

```
explorer [path]          open a folder (a file: its folder with the file selected); no path: the home folder
explorer --selftest      model and job engine tests on the console (no window server needed)
```

Start menu: **Gensokyo Explorer**; the panel's folder button and the desktop icons run `explorer`, `explorer <home>`,
`explorer /Netherworld` and `explorer /`. Window title: `Explorer - /tmp/x`. Design notes, platform facts and the problems found
along the way are in [EXPLORER.md](EXPLORER.md); the manual page is `man explorer`.

* **Window**: menu bar (File, Edit, View, Go, Help, keyboard driven like the Notebook's), toolbar (back, forward, up, refresh,
  address field, list and icon buttons, new folder), places (Home, the root, the Netherworld, and the volumes of `/proc/mounts`),
  the folder, and a status bar (items, hidden, selection and its size, messages, free space from `statfs`).
* **List view**: Name, Size, Modified, Type, Permissions, Owner (columns drop out from the right when the window is narrow);
  click a heading to sort, again to reverse; folders always first, natural order for numbers. **Icon view**: 96 x 84 cells with
  two-line labels. Only the rows or cells on screen are drawn. Hidden files (Ctrl+H), the view and the sort are remembered in
  `~/.config/explorer.conf`.
* **Folders are read in slices** (about 20 ms per frame) into one model and sorted at the end, so a folder of thousands of
  files appears at once and the window answers all the time. Refreshing (F5, after every job, and by itself when the folder's
  time stamp changes) reads into a second model and swaps, so nothing flickers and scroll position and selection stay.
* **Selecting**: click, Ctrl+click, Shift+click, a rubber band (also with Ctrl/Shift; scrolls at the edges), Ctrl+A, the arrows with
  Shift/Ctrl, Home, End, Page Up/Down, type-ahead find (letters typed within a second), Esc.
* **File jobs** (`fileops.c`): copy, move, send to the Netherworld, delete for good, restore and measure are jobs on a queue, worked
  in slices of one directory entry or 64 KiB. A job that lasts more than 0.4 s gets a progress window with Cancel (a cancelled copy
  removes its half-written file); errors are collected and listed once at the end; a name that is taken asks (Keep both, Replace,
  Skip, all with "do this for all"; Esc cancels); folders of the same name merge; copies keep permissions and modification times.
  Move tries `rename`, and on `EXDEV` (another volume, such as `/tmp` against the home folder) copies and removes each source item
  right after its copy succeeded. A folder cannot go into itself; `/`, `/Netherworld` and mount points cannot be moved or deleted.
* **The Netherworld**: Delete renames into `/Netherworld` (copying across volumes, refusing files too big for the free memory) under a
  free name (`name`, `name.1`, ...) and writes `/Netherworld/.trashinfo/<name>.info` (`path=` and `time=`). Inside it a banner offers
  Restore (parent folders are recreated; things without a note go to `$HOME`) and Empty it; Delete for Good and Empty ask first.
* **Clipboard**: Ctrl+C / Ctrl+X / Ctrl+V; a file in `/tmp` (`.explorer-clip-<uid>`), so it also works between two windows. Cut items
  are drawn faded. **Drag and drop** onto a folder of the list, a place or the Netherworld: move on the same volume, copy across
  volumes, Ctrl copies, Shift moves (between two Explorer windows is not done).
* **Rename** in place (F2; New Folder and New File start it too), **Properties** (Alt+Enter: type, location, size measured in the
  background for folders, times, volume, permission check boxes with chmod, owner and group with chown for the superuser), a
  **context menu** drawn inside the window (the server cannot say where a moved window is, so there are no popup windows).
* **Open**: folders are entered, text (by extension or by looking at the first bytes) goes to `notebook`, `.ppm .pgm .pbm .bmp` to
  `sketchbook`, programs run in `gapterm -e` (waits for Enter at the end), anything else asks to open it as text.

| Keys | Action |
|------|--------|
| Enter, Backspace, Alt+Left, Alt+Right, Alt+Up, Alt+Home | open, up, back, forward, up, home |
| F5, Ctrl+L / F6 / Alt+D, Ctrl+H, Ctrl+1, Ctrl+2 | read again, address field, hidden files, list, icons |
| Delete, Shift+Delete, F2, Alt+Enter | to the Netherworld, delete for good, rename, properties |
| Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A | copy, cut, paste, select all |
| Ctrl+N, Ctrl+Shift+N, Ctrl+Shift+F, Ctrl+W | new window, new folder, new file, close |
| Alt+F E V G H, F10, F1 | menus, the list of keys |

Notes for developers: source in `user/bin/explorer/` (`util.c` paths, formats and file types; `model.c`; `fileops.c` and `trash.c`
the job engine, which needs no window and is what `--selftest` exercises; `view.c`; `menus.c`; `dialogs.c`; `main.c`).
`EXPLORER_TRACE=1` prints `ex: ...` lines on stdout (folder read, selection, sort order, jobs, dialogs) that the tests wait for.
Tests: `python tests/run_all.py explorer` (`tests/case_explorer.py`): the self test (about 120 checks, also across volumes), navigation
and sorting, selection and keyboard, the Netherworld, copy / move / conflicts, drag and drop, rename and properties, a folder of
3000 files, and opening in the other applications.

## Developer tools

* `kappa-warp X Y` puts the pointer at an exact position through the window server (`FM_WARP_POINTER`) and returns when the
  server has done it. Real mouse clicks injected afterwards land there. `tests/gui_session.py` moves relative to the screen
  centre, which gets unreliable when the machine under test is slow; warping does not.
* `tests/case_apps.py`: `apps_cli` (engines and parsers through the text modes) and `apps_windows` (all five windows driven
  with the keyboard on a desktop boot, judged by their `--trace` lines and by pixels). Screenshots of the final state are
  saved as `build/t-apps-<app>.png`.
