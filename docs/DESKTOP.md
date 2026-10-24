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
Ctrl+F12 (screenshot: `~/Pictures/Screenshot-<date>-<time>.png` and `/tmp/screenshot.ppm`; `FM_SCREENSHOT` takes a path ending in `.png`,
`.bmp` or anything else for PPM, or `""` for both defaults, see `MEDIA.md`).

## Pictures: wallpaper, screenshots, thumbnails, the Gallery

`user/lib/kappa/include/kappa_media.h` (on top of libimg, see `MEDIA.md`) is what desktop programs use for pictures:

* **Wallpaper**: `~/.config/hakurei/wallpaper` names a picture and a mode (cover, fit, center, tile, stretch). `k_wallpaper_render()` draws it (or the
  built-in night scene when nothing is set or the picture cannot be read) and `k_wallpaper_stamp()` changes when the setting or the picture file changes:
  `hakurei-shell` re-renders when it does, `hakurei-lock` and `hakurei-greeter` call the same render function. `wallpaper IMAGE`, the Gallery's W key and the Control Panel (Appearance, Wallpaper) set it.
* **Screenshots**: `k_screenshot_path()` makes the `~/Pictures/Screenshot-...png` name; `fairy_screenshot(display, path)` asks the window server for the picture.
* **Thumbnails**: `k_image_thumbnail(path, size, &canvas)` (cached) for file browsers; `k_image_thumbnail_cached()` for painting without reading.
* **Gensokyo Gallery** (`user/bin/gallery/`, start menu entry in `hakurei-shell/launch.c`): the viewer; `gallery FILE` opens a picture and the folder it is in.

### How a frame reaches the screen

The compositor paints into a RAM back buffer and copies only what changed to `/dev/fb0`. The framebuffer is display memory (on a PC
mapped write-combining), so it is only ever written, never read, and always with wide stores. Pending damage is a list of up to 12
rectangles (overlapping or touching ones are merged when that wastes little), so a pointer in one corner plus a clock tick in the
other is two small repaints and not one full-screen one. The copy uses 16-byte non-temporal stores and ends the frame with an
`sfence`. `/tmp/fairyd.stat` (rewritten every two seconds) shows what frames cost: compose and flush microseconds per frame, the
worst frame, the flush speed in MB/s. `fairyd.copy=rep` and `fairyd.damage=bbox` on the kernel command line select the old copy and
the old single bounding box, for comparing (see [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md)).

## Hi-dpi screens (the ui scale)

The 8x16 font is tiny on a 2560x1440 monitor, so the window server has an integer **ui scale**: at scale N every logical pixel is
an N x N block of screen pixels and the desktop looks like a 1280x720 one, only sharper where there is something to gain.

* **When**: `fairyd` decides once at start (`choose_scale()` in `user/bin/fairyd/comp.c`). Automatically it is **2 when the screen is
  at least 2560 wide or at least 1440 high** (27-inch 2560x1440, 4K), else 1, so 1920x1080 and everything QEMU usually gives stay at 1.
  `ui.scale=N` on the kernel command line forces N (1 to 4); `ui.scale=1` switches the automatic choice off. A scale that would leave a
  logical screen smaller than 640x480 (the size every application promises to fit, see above) is lowered, e.g. `ui.scale=2` on 1280x800
  gives 1. `fairyd` logs `fairyd: ui scale N (automatic|ui.scale)`; `/tmp/fairyd.stat` and the Control Panel's Display page show it too.
  A screen size that does not divide by N leaves the last column or row black.
* **Coordinates are logical everywhere in the protocol**: the screen and work area in `FE_WELCOME`, window positions and sizes,
  `FE_CONFIGURE`, pointer events, commit rectangles, `fairy_warp_pointer()`. The window manager (`wm.c`) does not know about the scale
  at all. `FeWelcome.scale` carries it; clients read it with `fairy_scale()`.
* **The pointer** moves in screen pixels: mouse motion is added to a physical position (`S.ptr_x/ptr_y`) and the logical position is
  that divided by N, so a mouse feels the same at any scale and tests that move it by k pixels move it k screen pixels. The arrow and
  the other sprites are drawn at N times the resolution (same shapes, finer edges).
* **What the compositor draws itself** (frames, title bars, buttons, the Alt+Tab box, the background) goes through a Kappa canvas
  with scale N, so it is drawn at the finer resolution.
* **Client buffers**: a window created with `FW_HIDPI` has a buffer N times as wide and high as its logical size and is copied pixel
  for pixel. Without the flag the buffer is logical-sized and the compositor enlarges it, nearest neighbour (`draw_content_enlarged()`):
  this is the fallback for programs that draw their own pixels, and they need no change to work (input is logical, so clicks land
  where they should). At scale 1 `FW_HIDPI` is dropped by libfairy and nothing differs.
* **Kappa**: `KCanvas` has a `scale` field (0 or 1 means 1). On a scaled canvas `w`, `h`, the clip and every coordinate stay logical
  and all `kc_*` functions and widgets draw at the finer resolution: text and bevels as N x N blocks (the bitmap font has nothing
  finer), lines with a pen one logical pixel wide, circles, ellipses and gradients computed with the extra pixels, `k_icon()` drawn
  at N times its size, `kc_blit()` enlarging logical pictures and `kc_blit_native()` taking pictures that already have the canvas'
  resolution. `kc_native_view()` gives a 1x view of the same pixels for code that wants to draw finely itself. KApp windows ask for
  `FW_HIDPI` by default; a program that writes into `cv->px` itself calls **`kapp_set_hidpi(false)`** before its first window and gets
  1x windows enlarged by the server. `kapp_scale()` tells a program the scale either way. The `ks_*` blitters of `kappa_gfx2d.h`
  assume a 1x canvas.
* **Who does what at 2x**: the shell (panel, start menu, desktop icons), the lock screen and the login screen, the terminal, Explorer,
  Notebook, Calculator, Pocket Watch, Spell Manager, the Control Panel, Bunbunmaru News and the Kappa dialogs draw at full resolution;
  the wallpaper is rendered for the whole screen (`k_wallpaper_render_scaled()`: a picture uses every screen pixel, the night scene is
  drawn on a scaled canvas so it keeps its proportions). **Danmaku, Snake, Minesweeper, the Sketchbook and the Gallery** call
  `kapp_set_hidpi(false)` and are enlarged by the server: they look as they do at 1x, twice as large, and work the same (Gallery photos
  and Explorer thumbnails are therefore shown at logical resolution, enlarged).
* **1x is unchanged**: every scaled path is a separate branch taken only when the scale is above 1. `tests/pixel_identity.py` checks it bit
  for bit: it boots two builds at 1024x768 with the clock set to a fixed time and compares screenshots of the desktop, the start menu, a
  terminal and seven applications (Calculator, Notebook, Explorer, Minesweeper, Sketchbook, Control Panel, kdemo) byte for byte. Against
  `origin/main` at b8f0e84 and again at 395eb79 (after the job control and installer merges) every frame of every scene had an identical
  twin in the other run (2026-10-24). Pixels that show data rather than drawing are blanked in both runs first: the panel's CPU and memory
  meters, and the uptime, the memory in use and the free space of `/tmp` (these differ by 0.1 MB between builds of different size) on the
  Control Panel and in Explorer. The existing desktop and application tests pass unchanged.
* **Tests**: `tests/case_hidpi.py` (`hidpi_`): the 1x metrics, 2560x1440 (OVMF in QEMU offers it; start menu and terminal opened with
  clicks, title bar, glyphs exactly twice as large), `ui.scale=2` at 2048x1536 (the 1024x768 layout doubled) with a click into
  Minesweeper, `ui.scale=2` at 1920x1080 (where the automatic choice is 1) with 7 + 2 = clicked on the Calculator's keypad,
  `ui.scale=1` at 2560x1440 and `ui.scale=2` at 1280x800 (lowered to 1).
  Their `ScreenGui.home()` waits for the pointer to arrive in the corner before moving on: QEMU sums pending motion, and on a big,
  slow-to-paint screen the overshoot of the homing moves otherwise eats part of the way to the target.

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

## State when paused (hi-dpi, 2026-10-24, branch `cloud/hidpi`)

Done: the ui scale in `fairyd` (automatic at 2560 wide or 1440 high, `ui.scale=N`), logical protocol with `FW_HIDPI` buffers and the
nearest-neighbour fallback, scaled Kappa canvases, the shell, lock and login screens and the wallpaper at full resolution, the five
pixel-drawing programs opted out, `hidpi_` tests, the bit-exact 1x comparison (`tests/pixel_identity.py`). Verified in QEMU at 1024x768,
1280x800, 1920x1080, 2048x1536 and 2560x1440; merged with `main` after the job control and installer pull requests, and the desktop and
application tests pass on the result. Not seen on the owner's monitor yet. Compose time per full-screen frame at 2560x1440 under QEMU TCG is about 24 ms (four times the pixels of 1280x720);
on the Dell it should be far less, `/tmp/fairyd.stat` will tell.

## How to resume (hi-dpi)

- On the Dell: boot with `video=native` and check `fairyd: ui scale 2 (automatic)` in `dmesg`/the log and that the pointer speed feels right.
- The text console (kernel framebuffer console) is not scaled; a 16x32 console font for large screens would be the next step there.
- The opted-out programs could draw natively: the Gallery and Explorer thumbnails would gain most (decode at `size * kapp_scale()` and
  draw with `kc_blit_native()`); the games and the Sketchbook would need their own pixel code to take `kc_scale()` into account.
- `kc_blit_argb()` on a scaled canvas blends block by block (slow for big pictures); nothing uses it for large images today.

