# Gensokyo Explorer (`explorer`): work not started, design notes

Command `explorer [path]` (start menu name "Gensokyo Explorer"), a file manager for the desktop. **No application code
exists yet**; this branch only carries the shared GUI test helper `tests/guitools.py` (already in `main`) and this note.
Patchouli's Notebook and Marisa's Sketchbook were built by two sub-agents of this branch's agent and are merged
(branches `worktree-agent-a21c5271eaed80a2d` and `worktree-agent-ad1f2fdb71587f5e6`, see `docs/APPS_notebook.md` and
`docs/APPS_sketchbook.md`).

## State when paused

* **Done**: reading of the toolkit, window server and shell code; the design decisions below; `tests/guitools.py`
  (boot to the desktop, exact pointer positioning, click/drag/key helpers, `sh()` over the serial console, pixel
  checks), used by the Notebook and Sketchbook tests.
* **Half-done**: nothing.
* **Not started**: `user/bin/explorer/*.c`, `tests/case_explorer.py`, the popup-menu helper, the `docs/APPS.md` section.
* **Known problems** that affect the Explorer (found by the Notebook and Sketchbook agents and while reading):
  * No API gives a window's screen position after the user moved it (`fairy_window_geometry` is only filled when the
    window is created), so a popup window cannot be placed at the pointer without a trick (see "Context menu").
  * `kernel/drivers/keyboard.c` `mouse_byte()` passes the PS/2 wheel nibble through unchanged, so every app scrolls
    backwards with QEMU's mouse (fix: negate the wheel there). Not applied.
  * The system clipboard is 1024 bytes. KUI keeps 64 key presses per frame. `k_slider` ignores a click whose press and
    release land in one frame. `k_menubar` leaves the drop-down painted for one frame after a pick or Esc (call
    `kapp_redraw()`) and crashes on a NULL label. `k_input_dialog` does not preselect its initial text, the stock file
    dialog's first Esc only leaves the name field. `ui->double_click` is cleared after a double click (no triple click).
  * The guest clock lags when the host is busy and the PS/2 pointer occasionally drops packets under load: tests that
    need an mtime to change should `sleep` inside the guest, and GUI tests should retry once on a fresh VM.
* **How to resume**: start from a fresh branch off `main` (or this one), `python tools/build.py`, read `docs/DESKTOP.md`,
  `user/lib/kappa/include/kappa.h`, `user/bin/gapterm/main.c` and the Notebook (`user/bin/notebook/`, it has a keyboard
  driven menu bar, an in-window context menu and dialogs that do not freeze the window, worth copying). Suggested order: directory
  model (scan in slices, sort) -> list and icon views -> toolbar, places sidebar, address field, status bar -> navigation
  and keyboard -> selection (click, Ctrl, Shift, rubber band, Ctrl+A) -> file operation engine (trash, restore, copy,
  move with progress) -> context menu -> properties dialog -> drag and drop -> type-ahead -> tests -> `docs/APPS.md`.
  Look at every step with `python tests/gui_session.py script.txt prefix` and the Read tool on the PNGs.

## Design decisions already made

**Platform facts** (checked in `user/libc`, `shared/abi.h`, `kernel/fs`):
* `opendir/readdir` (with `d_type`), `lstat`, `rename` (fails with `EXDEV` across mounts, so move = copy + delete),
  `mkdir`, `rmdir`, `unlink`, `symlink`, `readlink`, `chmod`, `chown`, `getpwuid`, `getgrgid` exist. There is **no**
  `link()` (ENOSYS), no `utimes` (copies cannot keep mtimes) and **no `statfs`**: omit "free space" from the status bar.
* `/proc/mounts` lines are `source target type rw 0 0` (tmpfs `/`, devfs, procfs, tmpfs `/BambooForest`, plus FAT32
  volumes mounted later). `/tmp` is a symlink to `/BambooForest`, `/home` to `/HumanVillage`.
* The trash is `/Netherworld` (mode 1777). The `trash` applet of kappabox moves `/x/name` to `/Netherworld/name`,
  `/Netherworld/name.1`, ... and writes no sidecar.
* Launching other apps: `fork` + `execv("/bin/notebook", {"notebook", abs_path, NULL})` and the same for `sketchbook`;
  both take exactly one argv path (spaces, relative and non-existent paths work). The shell's own `launch()` splits on
  spaces and `chdir`s to `$HOME`; the desktop icons already run `explorer <home>`, `explorer /Netherworld`, `explorer /`.

**Model and speed**: scan with `readdir` + `lstat` in slices of about 25 ms per event-loop iteration (`kapp_set_tick` /
`ui->redraw_soon`), keep entries in one array plus cached lower-case names for sorting and type-ahead, cache
`getpwuid`/`getgrgid` results, draw only visible rows or cells (virtualised), `qsort` with directories first.

**Icons**: `k_icon` has folder, file, text, image, drive, trash, home but no executable, symlink or archive: draw
those in the Explorer (a `KI_FILE` base with a badge from `kc_*` primitives, a small arrow for symlinks).

**Trash and restore**: delete = `rename` into `/Netherworld` under a unique name; the original absolute path and the
deletion time go to a hidden sidecar `/Netherworld/.trashinfo/<trashed-name>.info` (hidden by the dot, shown with
"show hidden"). Restore reads it; items put there by the shell `trash` applet have no sidecar, so they restore to `$HOME`
with a message. "Delete permanently" and "Empty the Netherworld" ask first (`k_confirm_dialog`).

**File operations**: a job queue worked in slices (copy a chunk of 64 KiB per step, one directory entry per step), a
progress dialog (own `kwin_new` dialog with `k_progress` and a Cancel button) for jobs that take more than a moment,
errors collected and shown in one dialog at the end; `rename` first, on `EXDEV` copy then delete the source tree.

**Context menu** (the task asks for a popup window): the popup needs absolute screen coordinates, but the window's
position is only known at creation. Planned trick, untested: on the right click call `fairy_subscribe_windows()` again
(the server marks the list dirty on `FM_SUBSCRIBE` and re-sends `FE_WINDOWS` with every window's outer rectangle), find
the own window by `fairy_window_id()` in the event hook, derive the content origin as
`x = outer.x + (outer.w - content_w) / 2`, `y = outer.y + outer.h - content_h - (outer.w - content_w) / 2`, then open a
`FW_POPUP | FW_NO_DECOR` window there (clamped to the work area, flipped upward near the bottom). If no answer arrives
within about 400 ms fall back to an in-window overlay menu (what the Notebook does). Popups never take focus; `KApp`
forwards the owner's key presses to them, so arrows/Enter/Esc work. A shared helper would go into new files only
(`user/lib/kappa/src/ctxmenu.c`, `user/lib/kappa/include/kappa_ctxmenu.h`).

**Tests** (`tests/case_explorer.py`, QEMU names `t-explorer-*`): boot `text`, `Gui.start_desktop()`, create a tree with
`g.sh("mkdir -p /tmp/x/dir; echo hi > /tmp/x/a.txt; ...")`, run `explorer /tmp/x &` (first window content origin is
(44, 56)), assert on pixels and on the file system through `g.sh("ls ...")`: navigation, sort by header click, rubber
band, delete -> `/Netherworld` + sidecar -> restore, copy/move of a directory tree, EXDEV path (a second tmpfs mount is
`/BambooForest` versus `/`), thousands of entries (`seq`/`touch` loops) staying responsive, Enter opening a text file in
`notebook`.
