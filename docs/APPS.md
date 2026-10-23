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
first; `--dry-run` only prints what would happen) and **Appearance** (the theme palette, the 12 / 24 hour clock, and the **Wallpaper**: a picture
from the file dialog, one of five fit modes, or Reimu's night scene again; see [MEDIA.md](MEDIA.md)).
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

## Developer tools

* `kappa-warp X Y` puts the pointer at an exact position through the window server (`FM_WARP_POINTER`) and returns when the
  server has done it. Real mouse clicks injected afterwards land there. `tests/gui_session.py` moves relative to the screen
  centre, which gets unreliable when the machine under test is slow; warping does not.
* `tests/case_apps.py`: `apps_cli` (engines and parsers through the text modes) and `apps_windows` (all five windows driven
  with the keyboard on a desktop boot, judged by their `--trace` lines and by pixels). Screenshots of the final state are
  saved as `build/t-apps-<app>.png`.

## Gensokyo Gallery: `gallery`

The picture viewer (PNG, JPEG, GIF with animation, BMP, PNM, TGA): `gallery [FILE | FOLDER]`, start menu entry "Gensokyo Gallery". It walks the pictures of a
folder with the arrow keys, zooms with the wheel, pans by dragging, rotates, flips, runs slideshows and sets the wallpaper. Everything about it, the
libraries it stands on (libimg, libz) and its tests is in [MEDIA.md](MEDIA.md); `tests/case_media.py` drives it on a desktop boot (`media_gallery`).
