# Gensokyo Explorer (`explorer`): design notes

The file manager of the desktop: `explorer [path]`, start menu name "Gensokyo Explorer". The user-facing description (features,
keys, files) is the Explorer section of [APPS.md](APPS.md) and `man explorer`; this note records how it is built, the platform
facts it relies on, what the work turned up, and what is left. The first version of this note was written by an agent that only
did the research; the code (`user/bin/explorer/`), the manual page, the tests (`tests/case_explorer.py`) and this rewrite came
after.

## State

* **Done**: everything in the task list of the first note: a folder model read in slices, list and icon views (virtualised),
  sorting by column heading, toolbar, places (Home, root, Netherworld, volumes from `/proc/mounts`), address field, status bar
  with free space, selection (click, Ctrl, Shift, rubber band, Ctrl+A, keyboard, type-ahead), a sliced job queue (copy, move,
  send to the Netherworld, delete for good, restore, empty, measure) with a progress window, name conflict questions and one error
  list at the end, the Netherworld with restore notes, new folder / new file / rename in place, a context menu and a menu bar,
  Properties (chmod; chown for the superuser), drag and drop between folders and places, hidden files, a file clipboard that works
  between windows, opening text in the Notebook, pictures in the Sketchbook and programs in a terminal, preferences, the manual
  page, `docs/APPS.md`, and `tests/case_explorer.py` (a self test of the engine plus nine GUI sessions).
* **Not done**: drag and drop *between two Explorer windows* (see below), column resizing, a popup-window context menu, "Open
  with..." beyond the three fixed entries, searching, per-folder view settings, thumbnails.
* **Shared files touched**: `user/bin/gapterm/main.c` (new options `-e COMMAND` and `-t TITLE`: run a shell command line instead of
  a login shell, and the window title; the Explorer uses them to run programs). Nothing else outside `user/bin/explorer/`, the
  manual page, the tests and the docs.

## Platform facts (checked against the tree, October 2026)

* `opendir/readdir` (with `d_type`), `lstat`, `rename` (fails with `EXDEV` across mounts), `mkdir`, `rmdir`, `unlink`, `symlink`,
  `readlink`, `chmod`, `chown`, `getpwuid`, `getgrgid`, **`statfs`** (free space for the status bar and Properties) and **`utime`**
  (copies keep modification times) exist. There is no hard `link()`, no `lutimes` use for links, and **no `/bin/sh`**: a script
  with `#!/bin/sh` cannot be executed directly, so the Explorer runs such scripts as `gapsh script`.
* `tmp_readdir` walks the directory list from the start for every entry (the cookie is an index), so reading N entries costs O(N^2)
  list steps in the kernel. It is still fast enough (3000 entries in about 2 s in QEMU with the window answering throughout), and
  it is why folders are read in slices and why deletion reads the names of a folder before removing any of them.
* Mounts: `/` and `/BambooForest` (`/tmp`) are separate tmpfs mounts, so moving between `/tmp` and the home folder really goes
  through `EXDEV` (the test and the self test depend on that). A FAT volume appears in `/proc/mounts` like any mount and so in the
  places; the tests have no FAT disk, so that part was only read in the code, not run.
* `/Netherworld` is mode 1777 on the root tmpfs, that is, in RAM: trashing a big file from another volume could eat the memory, so
  copies into it are refused above half of what `statfs` says is free ("Too big for the Netherworld").
* Limits met on the way: 64 open files per process (children of the Explorer close everything above 2), 12 windows per `KApp`
  (dialogs are `kwin_new` windows of the same app), `exec` refuses more than 32 KiB of arguments (`xargs touch` of 3000 names
  needs `-n`), a 1 KiB system clipboard (file clipboard instead).
* Launching: `fork` + `execv` of `/bin/notebook`, `/bin/sketchbook`, `/bin/gapterm`, `/bin/explorer` with one argument each; the
  child does `setsid`, reads `/dev/null` and keeps stdout (so a test started from the console sees the children's traces).

## How it is built

**Model and speed** (`model.c`): `model_scan_step` reads entries with `readdir` + `lstat` for a time budget (20 ms per frame) and
`model_refilter` sorts when the scan ends (arrival order until then). Entries carry a folded name for sorting and type-ahead;
names compare "naturally" (file2 before file10); folders always sort first. `uid`/`gid` names are cached. The view draws and
hit-tests only the visible rows or cells. A refresh (F5, after a job, or by itself when the folder's time stamp changed or was
within a second of the scan) reads into a **second model** and swaps, so nothing flickers, the scroll position stays and a
selection is restored by name; a refresh that finds the folder gone walks up to the nearest folder that exists.

**Views and input** (`view.c`): list mode has a header with sort arrows; columns drop out from the right when the window is narrow.
Icon mode lays out 96 x 84 cells with two-line labels. A press on an entry selects it (Ctrl adds, Shift extends); a press on empty
space starts a rubber band that scrolls at the edges; a press that moves more than 4 pixels on an entry becomes a drag of the
selection. Dropping on a folder entry or a place calls `ex_start_drop` (same volume: move, other volume: copy, Ctrl: copy, Shift:
move, Netherworld: delete). Type-ahead collects letters for a second. Rename is a `k_textfield` laid over the name, preselecting the
name without its extension.

**Job engine** (`fileops.c`, `trash.c`; no window needed, exercised by `explorer --selftest`): a `Job` has top-level items and an
explicit stack of folders being walked; `job_run(job, budget_ms)` does one step (one entry or one 64 KiB chunk) until the budget is
spent, so jobs can stop anywhere (Cancel removes a half-written file). Errors are collected (200 are kept, all are counted) and shown
once. A name that is taken raises a question: the job waits until `job_resolve` answers it; "do this for all" sets a policy for the
rest. Move tries `rename`; across volumes and for merged folders it copies and removes each source item right after its copy
succeeded, so a failure leaves the rest where it was. Folders merge; a folder cannot go into itself; `/`, `/Netherworld` and mount
points are refused. Copies keep mode, owner (for root) and times.

**The Netherworld**: delete = move into `/Netherworld` under a free name (`NAME`, `NAME.1`, ... like the `trash` command) and write
`/Netherworld/.trashinfo/NAME.info` with `path=` and `time=`; Restore reads the note (recreating folders; a name that is taken asks;
no note: `$HOME`); Empty deletes everything including `.trashinfo`, which is made again by the next deletion.

**Clipboard**: `/tmp/.explorer-clip-<uid>` ("copy" or "cut", then one absolute path per line), polled twice a second, so copy in one
window and paste in another works; cut items are drawn faded.

**Context menu**: drawn inside the window (like the Notebook's) because `fairy_window_geometry` is only filled when a window is
created, so a popup window cannot be placed at the pointer after the user has moved the window; the plan in the first version of
this note (re-subscribing to the window list to learn the outer rectangle) was not needed. A menu that would leave the window is
moved back inside it. The menu bar is the Notebook's keyboard-driven one made general (`menus.c`).

**Dialogs** (`dialogs.c`): windows of the same `KApp`, modal ones block the file window (`X.modal`); the progress and conflict
windows follow the front job of the queue through `jobs_front()` and its serial number, never a pointer they kept, because the job is
freed the moment it ends.

## Problems found in shared code (not changed)

* No call reports a window's screen position after it was moved (see above); no drag and drop protocol in Fairies (a drag keeps
  delivering events to the window where it began, so a cross-window drop would need the window list, a z-order the server does not
  report, and a side channel such as a socket in `/tmp`).
* `k_textfield` identifies itself by the address of its buffer: the Explorer mirrors that formula (`field_id` in `main.c`) to know
  when the address field has the focus. `k_input_dialog` does not preselect its text and `k_confirm_dialog` has two buttons only, so
  the Explorer has its own choice dialog (like the Notebook).
* `KApp` marks every window dirty whenever a watched fd fires and redraws whole windows: a running job redraws the file window
  every 25 ms. It is fast enough, but a damage-aware draw would be kinder.
* The PS/2 pointer drops packets when the host is busy (the tests re-home the pointer and click again) and the guest clock lags, so
  timing tests (type-ahead expiry) cannot rely on wall-clock sleeps.
* `#!/bin/sh` scripts cannot run (no `/bin/sh`); a symlink to `gapsh` would fix it system-wide.

## Tests

`python tests/run_all.py explorer` (QEMU names `t-explorer-*`, about six minutes): `explorer_selftest` (123 checks of paths, formats,
model, and the whole engine including `/tmp` against the home folder as two volumes), `explorer_navigate_sort`,
`explorer_select_keys`, `explorer_netherworld`, `explorer_copy_move` (also a conflict question and the error list), `explorer_drag_drop`,
`explorer_rename_properties`, `explorer_progress_cancel` (a 39 MB copy, cancelled and then completed), `explorer_big_folder` (3000
files) and `explorer_open_apps`. The GUI tests read `ex:` lines from the console (`EXPLORER_TRACE=1`), look at the file system through
the shell and at pixels, take their coordinates from the layout constants documented at the top of `tests/case_explorer.py`, repeat a
click or drag whose effect does not show up (the pointer drops packets on a busy host), and run once more on a fresh VM before failing.

## How to resume

Ideas in rough order of value: drag and drop between two Explorer windows (each window could listen on a socket named after its pid
in `/tmp/fairy`-like space and the source could find the target through `fairy_subscribe_windows`; the missing piece is which window is
on top at the drop point); resizable columns; a real "Open with" chooser; a search box (the model already folds names); remembering
the view per folder; hard-link-free "Create link" (`symlink` exists); progress for `Properties` on huge folders in the status bar.
Start with `python tools/build.py`, `python tests/run_all.py explorer` and `python tests/gui_session.py script.txt prefix` plus a
look at the PNGs.
