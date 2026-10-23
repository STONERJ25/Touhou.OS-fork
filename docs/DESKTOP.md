# The desktop: Fairies, Kappa and HakureiDE

```
 apps (gapterm, explorer, notebook, ...)         user/bin/<name>/
        |  libkappa  (widgets, drawing, KApp loop)   user/lib/kappa/
        |  libfairy  (client library + protocol)     user/lib/fairy/
        v  AF_UNIX stream socket /tmp/fairy/fairyd.sock, shared pixel buffers in /tmp/fairy/*.buf
 fairyd  (window server + compositor)            user/bin/fairyd/   owns /dev/fb0 and /dev/input/events
 hakurei-shell (wallpaper, icons, panel, menus)   user/bin/hakurei-shell/
 hakurei-session (starts the two above)           user/bin/hakurei-session/
```

`init` starts `hakurei-session` for `reimu` when `/dev/fb0` exists (cmdline `text` or `nogui` skips it). `desktop` in the text
shell starts it by hand. Ctrl+Alt+Backspace kills the window server and returns to the text console.

## Writing an application

Any directory `user/bin/<name>/` with `.c` files becomes `/bin/<name>`; `user/lib/*/include` is on the include path and the
libraries are linked automatically. Look at `user/bin/kdemo/main.c` (a tour of every widget) and `user/bin/gapterm/` (a big
real application: pty, custom drawing, partial redraws).

```c
#include <kappa.h>

static void draw(KApp *app, KUI *ui, void *user) {          // called after every batch of events and on timers
    k_clear(ui);                                              // fill with the window background
    k_label(ui, 10, 10, "Hello, Gensokyo");
    if (k_button(ui, kr(10, 40, 100, 26), "Quit")) kapp_quit(app, 0);
}

int main(void) {
    KApp *app = kapp_new("Hello", 320, 200, FW_RESIZABLE);   // title, content size, window flags (see fairy.h)
    if (!app) return 1;                                       // no window server running
    kapp_set_draw(app, draw, NULL);
    return kapp_run(app);
}
```

* **Immediate mode.** `draw` rebuilds the whole UI every time. Widgets return true when activated; state (text buffers, list
  selections, scroll offsets) lives in your own variables. Draw order is paint order, later widgets are on top.
* **Input** is in `KUI`: `mx,my`, `buttons`, `pressed[]/released[]` edges, `wheel`, `keys[]` (this frame's key presses with
  `code` = KEY_* from `shared/keys.h`, `mods`, `ch` = Unicode), `key_down[]` (held keys, for games), `double_click`.
  Consume a key with `k_key_pressed(ui, KEY_ESC)`.
* **Timers and I/O**: `kapp_set_tick(app, ms)` redraws periodically; `kapp_watch_fd(app, fd, fn, user)` calls you when a file
  descriptor is readable (pipes, ptys, sockets). Set `ui->redraw_soon = true` for blinking carets and animations.
* **Dialogs**: `k_input_dialog()` (ask for text), `k_confirm_dialog()` (yes/no with an icon) and `k_file_dialog()` (open, save,
  choose folder) are non-blocking: they open a window in your `KApp`, close themselves and call your callback with the answer
  (NULL/false when cancelled). `k_messagebox()` is the blocking quick alert.
* **More windows**: `kwin_new()` creates popups (`FW_POPUP | FW_NO_DECOR`, closed by the server when the user clicks elsewhere)
  and dialogs in the same process. `k_messagebox()` is a quick modal alert.
* **Partial redraws**: call `k_damage(ui, rect)` for what you changed; only that part is copied and sent to the server.
  Without it the whole window is committed each frame.
* **Drawing**: `KCanvas` (`ui->cv`) has fills, lines, circles, gradients, bevels, `kc_text*` (8x16 bitmap font, UTF-8, one
  cell per code point), `kc_blit*`, and procedural icons (`k_icon`, 44 of them at any size). Colours are `0x00RRGGBB`.
  Palette names are in `kappa.h` (`KC_SHRINE`, `KC_PAPER`, `KC_NIGHT`, `KC_GOLD`...).
* **Clipboard**: `kapp_clipboard_copy()`, `kapp_clipboard()`; text fields and text areas already do Ctrl+C/X/V.

## Look and feel (HakureiDE)

Late-90s / PC-98 flavour, restrained. Window bodies are washi paper (`KC_PAPER`), accents are shrine red (`KC_SHRINE`) and
gold, dark backgrounds are night blue. Bevelled buttons, 8x16 bitmap text, no anti-aliasing, no animations that cost CPU.
Every app has a Touhou name for the start menu but a descriptive window title and plain-language labels. Keep the joke on top
of the mechanism, never instead of it (see `THEMING.md`). All art is procedural or original.

Conventions for applications:

* Window title = what the window is ("Terminal", "Notebook - report.txt"). Mark unsaved work with `*`.
* Resizable windows set a sensible `kapp_set_minsize()`; draw code must cope with any size.
* Menu bar (`k_menubar`, call it last in `draw`) for anything with more than a few commands; standard shortcuts: Ctrl+N/O/S/Q/W,
  Ctrl+C/X/V/A/Z/F, F1 help, Esc closes dialogs.
* Errors are shown with `k_messagebox`, never silently ignored.
* Applications must not assume a particular screen size (1024x768 in QEMU, whatever the GOP gives on real hardware) and must
  run fine at 640x480 content size.

## Window server protocol (short)

16-byte header (`size, type, pad, win, serial`) then a payload, see `user/lib/fairy/include/fairy.h`. A client connects, says
HELLO, creates windows by naming a freshly created shared-memory file (`/tmp/fairy/<pid>-<n>.buf`, ARGB-less 0x00RRGGBB
pixels), draws into the mapping and sends COMMIT with the damaged rectangle. The server draws frames and sends back key,
pointer, focus, resize (CONFIGURE) and close events. Shell clients (`fairy_register_shell`) also receive window lists
(`fairy_subscribe_windows`) and global hotkeys (Super = launcher, Ctrl+Alt+T = terminal, Ctrl+Alt+Del = Spell Manager).

Other keys handled by the server: Alt+Tab (switcher), Alt+F4 (close), Alt+drag (move), double click on a title bar (maximize),
Ctrl+F12 (screenshot to `/tmp/screenshot.ppm`).

### How a frame reaches the screen

The compositor paints into a RAM back buffer and copies only what changed to `/dev/fb0`. The framebuffer is display memory (on a PC
mapped write-combining), so it is only ever written, never read, and always with wide stores. Pending damage is a list of up to 12
rectangles (overlapping or touching ones are merged when that wastes little), so a pointer in one corner plus a clock tick in the
other is two small repaints and not one full-screen one. The copy uses 16-byte non-temporal stores and ends the frame with an
`sfence`. `/tmp/fairyd.stat` (rewritten every two seconds) shows what frames cost: compose and flush microseconds per frame, the
worst frame, the flush speed in MB/s. `fairyd.copy=rep` and `fairyd.damage=bbox` on the kernel command line select the old copy and
the old single bounding box, for comparing (see [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md)).

## Testing and looking at your work

`python tests/gui_session.py script.txt prefix` boots the OS in QEMU, types shell lines, drives mouse and keyboard, and saves
screenshots to `build/<prefix>-<name>.png` (open them with an image viewer; the Read tool shows PNGs). Script commands:
plain lines are typed in the text console, `@wait s`, `@shot name`, `@move dx dy` (relative; the pointer starts mid-screen at
512,384), `@down`/`@up`/`@click [left|right|middle]`, `@type text`, `@key qcode [ctrl] [alt] [shift]`, `@print`. Typical start:

```
fairyd &
@wait 1.5
hakurei-shell &
@wait 3.5
myapp &
@wait 3
@shot a
```

Automated tests go in `tests/case_<name>.py`; see `tests/case_desktop.py` (`Qemu.screen_ppm()` and `Qemu.pixel()` inspect the
framebuffer, serial output is available through `vm.text()`). A program's stdout/stderr when started from the panel goes to
`/tmp/<name>.log`.

## Sound

Programs make sound through libaudio (`#include <audio.h>`, linked automatically): `audio_open()` returns NULL when the machine
has no sound device, and every program must cope with that. `audio_event(AUDIO_EV_DIALOG)` (also `AUDIO_EV_LOGIN`,
`AUDIO_EV_ERROR`, `AUDIO_EV_SHUTDOWN`) plays one of the short system sounds and returns at once; it does nothing when the user
has switched the system sounds off (`/etc/hakurei/sounds` or `$HAKUREI_SOUNDS`). Status: the functions work, but the shell,
the greeter, the lock screen and the Kappa dialogs do not call them yet, and Reimu's Jukebox is not written. See
[AUDIO.md](AUDIO.md) for the design and for what is done.
