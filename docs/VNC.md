# VNC: the desktop from another computer

`vncd` (`user/bin/vncd/`) serves the desktop over RFB, the protocol of VNC, version 3.8 (RFC 6143), so that an ordinary viewer
(TigerVNC, RealVNC, TightVNC, `vncviewer`, Remmina) shows the screen and sends keys and mouse. In the theme it is Hatate's remote
window: she is the tengu who photographs places she is not at (`[vncd] Hatate keeps watch on 127.0.0.1:5900`).

It is off unless `vnc` is on the kernel command line. By default it listens on **127.0.0.1 only**: the way in is an ssh tunnel.

```
ssh -L 5900:127.0.0.1:5900 marisa@gensokyo          # on the other computer; keep it open
vncviewer localhost:5900                            # or TigerVNC / RealVNC with "localhost:5900"
```

## The three design questions

### 1. Getting the picture

`fairyd` composes the screen into a RAM back buffer (`S.back`, 0x00RRGGBB, physical pixels) and copies only the damaged rectangles
to `/dev/fb0` with non-temporal stores; the framebuffer is write-combining display memory and is never read back (DESKTOP.md, "How a
frame reaches the screen"). Reading it from user space would be slow and would also return the pointer, so `vncd` never touches
`/dev/fb0`. Instead `fairyd` has a **frame service** (`user/bin/fairyd/framepub.c`, protocol in `user/bin/fairyd/framepub.h`) on its
ordinary socket, in a message range (64.., 164..) that `fairy.h` does not use, so the toolkit's protocol and every existing client are
unchanged:

* `FM_FRAME_SUBSCRIBE` (only a peer with uid 0, checked with `getpeercred`; one subscriber) makes a shared-memory file
  `/tmp/fairy/frame-<pid>-<n>.buf` (0600, `O_EXCL|O_NOFOLLOW`) the size of the whole framebuffer plus a 128x128 sprite area, and
  answers `FE_FRAME_READY` with its name.
* `FM_FRAME_REQUEST` asks for the next frame. `fairyd` answers at the end of the main loop's round in which there is news (damage, the
  pointer moved or changed shape, the clipboard changed, a size change), or at once with `FRAMEREQ_NOW`. Only then does it copy
  pixels: the damaged rectangles of the back buffer, into the shared memory, followed by `FE_FRAME` (the rectangles, move hints, the
  pointer's position, shape serial and hot spot, the modifier state, a clipboard counter). Between `FE_FRAME` and the next request the
  shared memory is the subscriber's to read; `fairyd` never writes it then and **never waits for the subscriber**. While nobody asks,
  the service only merges damage into a 32-rectangle list (the same merging as the compositor's own list).
* **The pointer is not in the picture.** The back buffer no longer holds the pointer at all: `flush_rect()` draws the sprite into each
  row on its way to the framebuffer (`overlay_cursor_row()`), so the screen shows exactly the pixels it showed before, screenshots
  still include the pointer, and moving the pointer now costs no compositing, only the copy of the two small rectangles it left and
  entered (`comp_damage_pointer()`, a flush-only list). The sprite is published beside the pixels, so a viewer with the Cursor
  pseudo-encoding draws the pointer itself (no lag), and `vncd` draws it into the pixels for a viewer without. Checked with
  `tests/pixel_identity.py` against `main` at ea2fcec (2026-11-03): every scene (desktop, start menu, terminal, Calculator, Notebook,
  Explorer, Minesweeper, Sketchbook, kdemo) has a frame that is the same byte for byte; the Control Panel differs only in the memory
  figure ("28.9 MB used" against "29.1 MB", the initrd is larger), which is data, not drawing.
* **Move hints** for CopyRect: a window whose position changed since the last published frame while nothing else about it did (a
  signature of size, state, focus, title, flags, buffer and a commit counter), which was unobscured then and is now (no visible window
  or drop shadow above, no Alt+Tab box), shows the old pixels at the new place: the compositor draws a window relative to itself.
  `vncd` compares the pixels (`memcmp` of the new frame against its own copy of the old one) before it believes a hint.
* **Scrolling** has no hint (a terminal just redraws its window): when no window moved, `vncd` looks for rows of a damaged rectangle
  that are its old rows shifted up or down (`find_scroll()` in `source.c`: row hashes, probe rows that are not blank, at most eight
  shifts tried, at least 32 rows), compares the run pixel by pixel and sends it as CopyRect too.
* **Freshness**: a viewer's FramebufferUpdateRequest is answered from a frame the window server made after the request arrived
  (`source_fresh_ticket()`; a request with `FRAMEREQ_NOW`, or an outstanding request changed to answer now), never from an old copy.

`vncd` keeps its own copy of the frame (`g_frame`), takes the damaged rectangles over from the shared memory and tells each viewer what
changed (`clients_frame_changed()`). It asks for frames only while a viewer waits for an update, at most every 15 ms.

### 2. Injecting input

`/dev/vinput` (`kernel/drivers/vinput.c`, registered with `INITCALL`, so `devfs.c` is unchanged) takes whole `struct ginput_event`
records (`shared/abi.h`, 16 bytes) from root only (the node is 0600 root and the write checks the caller), checks every record
(EV_KEY key codes 1..255 with 0/1/2, the three buttons with 0/1; EV_REL X/Y at most 4096, the wheel at most 16 clicks; EV_ABS X/Y
0..65535) and refuses a write with one bad record before queuing any. Good records go through `input_emit()` into the queue that
`fairyd` reads from `/dev/input/events`, so the window server cannot tell them from a keyboard and a mouse. They do not pass the
console's key handling (they never type into the text console, and Ctrl+Alt+Backspace sent through VNC does not kill the window server).

**Absolute pointer**: the compositor only knew relative motion (the tests home the pointer in a corner and walk it). `fairyd` now takes
`EV_ABS` `ABS_X`/`ABS_Y` (Linux's numbers 0 and 1, defined in `fairyd/main.c` and `vncd/port.c`) as a position in physical pixels,
batched like relative motion (`cursor_move_phys()`); RFB pointer positions are framebuffer pixels, which are physical pixels, so at ui
scale 2 nothing is converted.

**Keys** (`user/bin/vncd/keys.c`): X11 keysyms become the system's key codes (PS/2 set 1, `shared/keys.h`). `fairyd` turns codes into
characters with the US layout, so a printable keysym becomes the US key that types it plus the shift that key needs, and `vncd` presses
or lifts shift around that one key when the viewer's shift state says otherwise (a German keyboard's Shift+7 arrives as Shift_L and
the keysym `/`, the unshifted US slash key; the server lifts shift for it). Caps Lock as the window server sees it (published in each
frame) turns letters round. Function keys, the keypad (with the viewer's Num Lock off, KP_Home is Home), modifiers, Super, Menu are
mapped; Unicode keysyms count as far as ASCII reaches; anything else is dropped (one log line per viewer). A key held twice is a repeat
(value 2) for the window server; keys and buttons a viewer holds when it goes are released. Buttons 1 to 3 are left, middle, right;
4 and 5 are one wheel click up and down per press.

### 3. Authentication and exposure

* **Listening**: 127.0.0.1:5900 by default. `--listen-all` / `vnc.listen=all` listens on every interface, logs a warning, and then
  a password is required: without one every viewer is turned away with a reason.
* **Security types**: "None" only when listening on 127.0.0.1 and no password is set (then the only ways in are an ssh tunnel, which
  already took a key, and programs on this machine). With a password, "VNC Authentication" only, on loopback too.
* **VNC Authentication** is DES encryption of a 16-byte random challenge with the password as key (RFC 6143 7.2.2), with the quirk that
  the bits of each key byte are reversed. DES is written from FIPS 46-3 in `vncd/des.c` (checked against four published known-answer
  vectors and against OpenSSL); the comparison of the answer is constant time. It is weak (56 bits, at most 8 characters, and a recorded
  session can be attacked offline), which is why the default is loopback plus ssh.
* **The password file**: `/etc/vnc.passwd`, one line of 1 to 8 printable ASCII characters (VNC needs the password itself, not a hash),
  root's, 0600, read at every connection. A file that others may read, that is not a plain file of root's or that is damaged is not
  used and every viewer is turned away ("fail closed": a broken file never means "no password"). `vncd -passwd` writes it atomically and
  keeps a copy in `/HumanVillage/.system/etc/vnc.passwd` (root's on every FAT mount, like Keine's records); `vncd` puts the copy back
  into `/etc` when it starts on a fresh `/etc` (`[vncd] the VNC password was restored from the Human Village's records`).
  `vncd -nopasswd` removes both. (It is not in `etc_kept_names`: init and `etckeep` do not know it; see "Open points".)
* **Throttle**: a wrong password is answered after one second; three wrong ones from an address within a minute turn that address
  away for a minute, with a reason in the protocol and one log line (`vnc.throttle=N`, 0 off).
* **Limits**: 8 viewers (`vnc.maxclients`), 4 per address (`vnc.peraddr`), 30 s for the handshake, 60 s for a viewer that stops reading
  (`vnc.stall`), a refusal in the protocol for anything over the limits and a plain close for a flood.

## Using it

1. Put `vnc` on the kernel command line (`cmdline.txt` on the ESP). `init` starts `/bin/vncd`, which restarts itself if it ends.
2. Optionally `sudo vncd -passwd` (needed for `vnc.listen=all`).
3. From another computer: `ssh -L 5900:127.0.0.1:5900 marisa@ADDRESS`, then a viewer on `localhost:5900`. On the dev PC `python tools/vncshot.py --tunnel ADDRESS --keep` opens that tunnel with the dev key and the pinned host key and closes it on Ctrl+C (no viewer is installed there by default: TigerVNC from its own site works). The `direct-tcpip` forwarding
   in `sshd` is another agent's work; until it is merged the tunnel needs `vnc.listen=all` and a password on a trusted network instead.

| Option | Kernel command line | What |
|--------|---------------------|------|
| `--port N` | `vnc.port=N` | another port than 5900 |
| `--listen-all` | `vnc.listen=all` | every interface (warning; password required) |
| `--allow-resize` | `vnc.resize` | viewers may change the desktop size (SetDesktopSize) |
| | `vnc.throttle=N` | wrong passwords per minute before an address waits (3; 0 off) |
| | `vnc.peraddr=N`, `vnc.maxclients=N` | viewers per address (4; 0 no limit), in all (8) |
| | `vnc.stall=SECONDS` | how long a viewer may stop reading (60) |
| `--verbose` | `vnc.verbose` | a log line for every connection, and the encodings and pixel format each viewer asks for |
| `-passwd`, `-nopasswd` | | set or remove the password (root) |

**The Dell** (2560x1440, ui scale 2): viewers get the physical 2560x1440 frame (the desktop drawn at 2x). TigerVNC can scale the window
down. Over gigabit any encoding is fine; ZRLE is what TigerVNC picks by itself and costs least on a slow link.

**Resolution changes**: the firmware's mode is fixed after boot, so a size change is done in software: with `--allow-resize` a
viewer's SetDesktopSize (TigerVNC's "resize remote session") asks `fairyd` (`FM_SET_MODE`, declared in `fairy.h` long ago and served
now, for root peers only) to use a w x h part of the framebuffer, top left, at the same ui scale (between 640x480 and the framebuffer). The rest of the
monitor is black, which is why it is off by default. Every viewer is told (ExtendedDesktopSize or DesktopSize, then a full update); a
viewer that announced neither is closed.

## Protocol

* Versions 3.8, 3.7 and 3.3 (3.4 to 3.6 as 3.3, as RFC 6143 says); the ProtocolVersion is checked byte for byte.
* Security types 1 (None) and 2 (VNC Authentication); SecurityResult with a reason in 3.8; refusals with a reason (3.3: type 0).
* ClientInit's shared flag (0 closes the other viewers); ServerInit: 32 bpp, depth 24, little endian, 0x00RRGGBB, name
  "gensokyo (TouhouOS)".
* Client messages: SetPixelFormat (8, 16 or 32 bpp; any true-colour layout whose maxima are 2^n-1 and whose channels fit and do not
  overlap; an 8-bit colour map, served with the server's BGR233 cube through SetColourMapEntries), SetEncodings (at most 512),
  FramebufferUpdateRequest (incremental or not; areas outside the screen are clipped), KeyEvent, PointerEvent, ClientCutText (at most 1
  MiB, kept up to the clipboard's 1024 bytes, Latin-1 to UTF-8), SetDesktopSize (only after ExtendedDesktopSize was announced). Any
  other message type ends the connection.
* Server messages: FramebufferUpdate, SetColourMapEntries, ServerCutText (the clipboard changed; UTF-8 to Latin-1, '?' for the rest).
* Encodings: Raw, CopyRect, Hextile, ZRLE (one zlib stream per connection, `user/lib/z`, sync-flushed per rectangle; the compression
  level pseudo-encodings -256..-247 pick the level before the stream starts, 1 by default). Pseudo-encodings: Cursor (-239),
  DesktopSize (-223), ExtendedDesktopSize (-308). The first of Raw, Hextile and ZRLE in the viewer's list is used.

### How an update is made (`user/bin/vncd/client.c`)

Each viewer has two regions (`region.c`: y-x banded rectangle sets): `mod`, pixels to send, and `copy`, a pending CopyRect (all with
one shift). A verified move hint splits the moved region into: pixels whose old place the viewer has right (copy with the move's
shift), pixels whose old place is itself a pending copy (copy with the two shifts added), pixels whose old place the viewer lacks
(send). One shift can be pending, so the largest kind wins and the rest is sent; sending pixels is always correct, so every doubtful case
does. An update is CopyRect rectangles first (ordered so that none overwrites a later one's source), then the pixels. A viewer without
the Cursor encoding has the pointer in its pixels, so the pointer's old and new places are sent and a copy over it draws it again.

Flow control is RFB's own: a viewer gets an update only when it asked and less than 256 KB of the previous one is waiting in the socket;
what changes in between accumulates in its regions (simplified to at most 48 rectangles). A viewer that stops reading therefore costs
one update of memory and is dropped after `vnc.stall`; nothing it does slows the others or `fairyd`.

## Tests

**Host** (no QEMU; the Linux build of `vncd` with AddressSanitizer and UBSan against a fake window server that speaks the frame
protocol, `tests/host/vnchost.py`): `python tools/hosttest.py discover -s tests/host -p "test_vnc.py" -v`. 37 tests: the handshakes
in three versions, passwords right and wrong, the throttle and its delay, password files that must fail closed, `--listen-all` without a
password, exclusive viewers; Raw, Hextile and ZRLE in five pixel formats (16-bit big-endian 565, 8-bit BGR233, 32-bit big-endian with
the colours high, 15-bit, the colour map) against the fake's picture, pixel for pixel; impossible pixel formats; incremental and partial
updates; CopyRect for moves, for a slow viewer that collects several moves, for moves gathered into one frame, refusing a false hint,
with the pointer drawn into the pixels, for a scroll found without a hint; the cursor shape and its overlay; DesktopSize, ExtendedDesktopSize and SetDesktopSize (refused,
invalid, done); keys (shift added and lifted, repeat, function keys, keypad, unknown keysyms, release at disconnect) and Caps Lock;
pointer and wheel; the clipboard both ways; 50+ hostile streams; the handshake timeout; a viewer that never reads; 100 reconnects; the
window server going away and coming back; no frame requests while nobody watches; a fake window server offering `/etc/passwd`.

**QEMU** (`python tools/qemu-slot.py -- python tests/run_all.py vnc_`, `tests/case_vnc.py`):

| Test | What it shows |
|------|---------------|
| `vnc_desktop_view` | password right, wrong and missing; full updates in Raw, Hextile, ZRLE and 16-bit 565 equal to QEMU's screendump pixel for pixel (and with the viewer's own pointer drawn in); an incremental update covering a new window; the arrow and the terminal's I-beam as cursor shapes; throughput |
| `vnc_input` | Ctrl+Alt+T and a typed command through VNC run in a terminal (the guest's log shows `typed-42-Ok`); the viewer's clipboard pasted with Ctrl+Shift+V runs too, and a word double-clicked in the terminal comes back as ServerCutText; dragging a window's title bar gives CopyRect and the right picture; a click on the start button opens the menu |
| `vnc_resize` | `vnc.resize`: SetDesktopSize 800x600; the asker gets ExtendedDesktopSize reason 1, a DesktopSize viewer the new size; both pictures equal the screen's top left, the rest of the monitor is black, the panel moved; and back to 1024x768 |
| `vnc_robustness` | 50 hostile streams before and after a login; the picture is still right; a viewer that never reads while the compositor keeps drawing and another viewer keeps getting updates, dropped after `vnc.stall=10`; 30 reconnects; the supervisor never had to restart vncd |
| `vnc_throttle_and_defaults` | the default listens on 127.0.0.1 (the host cannot connect, `nc` in the guest sees `RFB 003.008`; the log says that without a password a local viewer needs none, which the host tests check through the protocol); `vnc.throttle=3`: three wrong passwords turn the right one away too |
| `vnc_password_kept` | `vncd -passwd` writes `/etc/vnc.passwd` 0600 root and the volume's copy (read back on the host); after a restart vncd restores it and the same password works |
| `vnc_desktop_of_an_ordinary_user` | the window server and the shell run as `reimu` (the way init's session runs them) while `vncd` is root: the viewer's picture equals the screen. Found on the Dell on 2026-11-03: the other tests ran the desktop as root, so they never saw that a client's `getpeercred()` on a UNIX socket returned root for the server until the server had called `accept()`; `vncd` took that for the window server's user, found the frame file owned by someone else and turned it away (the viewer got an empty 1024x768 desktop). The kernel now records the listener's credentials at `listen()` and gives them to a client at `connect()`; `utest` checks it before `accept()` |
| `vnc_idle_uses_no_cpu` | `vncd` sleeps when nobody is watching, with an idle viewer, and after it left (its CPU time over six seconds, read from `/proc/PID/stat`). Found on the Dell the same afternoon: `source_next_deadline()` returned "15 ms after the last frame" even when no frame was wanted, that time was in the past, `poll()` got a timeout of zero and `vncd` ran at 100% of a core for 40 minutes (9 W and 50 C at an idle desktop instead of 3.7 W and 32 C); fixed by returning a deadline only when a frame is wanted |

**Measured** (`vnc_desktop_view`, QEMU TCG on the development PC, 1024x768 desktop with the panel and one window, user networking, the
time includes the Python viewer's decoding, other agents' guests running at the same time): one full update is **Raw 3072 KB in
0.85 to 2.84 s** (two runs; about 1 to 3.6 MB/s through QEMU's user networking, depending on how busy the PC was), **Hextile 251 KB in
0.62 to 0.70 s** (the Python decoder is the slow part) and **ZRLE 69 KB in 0.12 s** both times (45 times smaller than Raw). An incremental update for a new window was a few tens of KB; a stuck viewer did
not slow the compositor (139 frames drawn in 14 s while it was stuck, the other viewer got 66 updates). vncd's own encoding time is in
its log line when a viewer goes (`... N updates, X KB sent, Y ms spent encoding`; a TightVNC session on the host build: 61 updates,
16 ms). Not measured on the Dell yet.

**A real viewer**: the TightVNC Viewer installed on the development PC (`C:\Program Files\TightVNC\tvnviewer.exe`) was tried
(2026-11-03, by script, not part of the suite because it needs a Windows desktop) against the host build and against the real desktop in
QEMU (`vnc vnc.listen=all vnc.verbose`, through the port forward): it logged in with the password, asked for
`7,1,16,5,2,0,-26,-223,-224,-232,-239` (Tight first, which vncd skips), was served ZRLE with CopyRect and the Cursor, showed the desktop
(wallpaper, a terminal, the terminal's I-beam drawn by the viewer itself), and text typed into its window ran as a command in the
guest's terminal (`logger` output on the serial line). 141 updates, 931 KB, 350 ms of encoding in vncd for that session.

**Regression** (2026-11-03, this branch): `desktop_`, `hidpi_`, `lock_screen`, `login_screen`, `hwaudit_fairyd_frame_stats`,
`sketchbook_screenshot` and `media_screenshot` pass with the changed compositor, and `tests/pixel_identity.py` finds every scene bit for
bit the same as on `main` (see "Getting the picture").

## Open points and future work

* **Unverified on real hardware and with most viewers.** The tests' viewer is strict and written from the specifications and TightVNC's
  viewer works (above), but TigerVNC (and its remote resize), RealVNC and macOS's Screen Sharing have not been tried, nor the Dell
  (2560x1440 at ui scale 2; the frame service's copy of a full frame there is 14.7 MB of memcpy per update that needs it).
* The shell caches the screen size at start: after a SetDesktopSize the panel and the desktop follow (fairyd moves and resizes them),
  but the start menu and the calendar still open where the old size put them (`hakurei-shell/panel.c` uses `G.screen_w/h`; it would
  need to read `fairy_screen_size()` again on FE_CONFIGURE of its desktop window).
* `vnc.passwd` is kept by vncd itself; adding it to `etc_kept_names` (`user/libc/src/etcstore.c`, with mode 0600 and a one-line check)
  would let init restore it and `etckeep` show it.
* Only vertical scrolls are found (sideways scrolling is sent as pixels), and only one copy per frame is sent (the largest of a
  window move and a scroll).
* Not implemented: Tight, TRLE, RRE and Zlib encodings; the extended clipboard (UTF-8, `0xC0A1E5CE`); continuous updates and fences;
  QEMU's extended key events (raw scan codes would make non-US layouts exact); keyboard layouts other than US; file transfer; audio;
  TLS-encrypted VNC (VeNCrypt) and other security types. Viewers use what they are offered, so none of this breaks them.
* The ABS_X/ABS_Y numbers are defined locally in `fairyd/main.c`, `vncd/port.c` and `kernel/drivers/vinput.c`; they could move to
  `shared/abi.h` next to `EV_ABS`. The frame service's message numbers live in `fairyd/framepub.h`, outside `fairy.h`.

## Seen on the Dell (2026-11-03)

First run on the real machine, build `1b6c1ec`, `vnc` added to `cmdline.txt` with `push-update.py --cmdline` (loopback only, no password: the way in is the ssh tunnel).

* **The tunnel works**: `ssh -L PORT:127.0.0.1:5900 marisa@192.168.1.185` through the Dell's own sshd (`direct-tcpip`, policy `local`). `python tools/vncshot.py --tunnel 192.168.1.185 --out build/dell.png` opens one, takes a picture and closes it (no viewer is installed on the dev PC; it uses the test viewer `tests/rfbclient.py`).
* **First attempt failed**, and only on the real machine: the viewer got an empty 1024x768 desktop although the screen showed the real one. The log said `the frame file ... is not what the window server said (size, owner or kind)`. Cause: `vncd` asks `getpeercred()`
  right after `connect()`, the kernel filled the accepting end's credentials only at `accept()`, so `vncd` was told "root" for the window server, which runs as `reimu` under init's session, and rejected the frame file `reimu` owns. The VNC tests had started the desktop as root, where
  the zero default happens to be right. Fixed in the kernel (`kernel/fs/socket.c`: the listener's credentials are recorded at `listen()` and given to the client at `connect()`), `utest` checks it, and `vnc_desktop_of_an_ordinary_user` runs the desktop as `reimu` (it fails on the old kernel).
* **After the fix**: a full 2560x1440 frame in 0.4 s (127 KB, ZRLE, over the tunnel and the home Wi-Fi); Ctrl+Alt+T opened a terminal on the Dell's desktop, a typed command ran as `reimu` and its output was on the next picture. The pictures are in `build/dell*.png` (git-ignored).
* **A second bug, found by looking at `ps` afterwards**: `vncd` used a whole core from the first minute (`/proc/PID/stat` showed 1000 ms of CPU per second; `sensors` said 9 W and 50 C). It spun in its own main loop (see `vnc_idle_uses_no_cpu`); the fix is in `source.c`.
* Not tried on the Dell: `vnc.listen=all` with a password, `vnc.resize`, a real viewer (TigerVNC), a second viewer, long sessions.
