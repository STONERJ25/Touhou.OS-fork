# The PC-98 application ("Yukari's Gap to PC-98"): the games in a window on the desktop

Branch `agent/pc98-app` (M8 of [PC98.md](PC98.md)). Source: `user/bin/pc98app/` (`/bin/pc98app`, also `/bin/pc98`). The emulator itself is the library `user/lib/pc98` ("Hakurei emulator":
[PC98.md](PC98.md), [PC98_BIOS.md](PC98_BIOS.md), [PC98_GFX.md](PC98_GFX.md), [PC98_SOUND.md](PC98_SOUND.md)); this page covers what the application adds, how to use it, what is tested and how to resume.

**The rule, again:** the owner's disk images, font ROM and drum samples are read at run time from where the owner put them (their own volume, `assets/local/` on the development PC)
and are never copied into the tree, embedded, quoted or committed. The tests use images and programs they make themselves.

## Using it

Start it from the start menu (**Yukari's Gap to PC-98**, in Games), from the desktop icon (**PC-98**) or from a terminal:

```
pc98 [options] [IMAGE ...]            pc98 /HumanVillage/pc98/th01.hdi
pc98 --font /HumanVillage/pc98/FONT.ROM --rhythm /HumanVillage/pc98/rhythm th04.hdi
pc98 --bench 40 --bench-key 15.6:1c ... th01.hdi         (no window: how fast it runs, see "Speed")
```

*Why Yukari:* the emulator is a gap to another world, the one the games were made for; Yukari opens gaps (and in TouhouOS her tag already stands for gaps, pipes and the like). The window title is the plain fact: `PC-98 - th01.hdi` (a `*` after the name when the guest has written to its disk).

**Images.** D88, FDI, HDI and raw floppy dumps (pc98disk.h). A hard disk image (HDI) goes to hard disk 1, anything else to floppy 1, then floppy 2 (DA/UA 80h, 90h, 91h; hard disk 2 and the other units are there too). The machine boots from floppy 1 when it has a disk, else from hard disk 1; *Reset from the hard disk*
does the other. File menu: *Insert floppy 1 / 2...*, *Open hard disk image...* (a Kappa file dialog; a hard disk image or a restart for a hard disk boots at once, a floppy put into a running machine is just there), *Eject*, the last five images used (`~/.config/pc98/recent`, eight are kept).

**The disk is never written.** Each image is read into memory whole and the guest works on that copy (the HLE BIOS writes sectors into it). The file is not opened for writing again by anything. *Save changes as...* writes the copy to a **new** file (the dialog proposes `name-saved.ext`, or `-saved-2`, ... whichever does not exist;
a name that exists is refused: `O_EXCL`, nothing is overwritten, the original included) and the new file is the image's file from then on. *Discard changes and restart* reads the file again. Closing the window (or Quit, or Alt+F4) with unsaved changes asks: *Save as...*, *Discard and quit*, *Keep running*. What "changed" means is exact to the 512-byte block
(a checksum per block is taken at load and at save): a guest that writes the bytes it read has changed nothing. The status line and the title show unsaved changes.
**TH04's CONFIG.SYS:** it loads `HIMEM.SYS` and `EMM386.EXE /UMB` (V86 mode, not emulated) and wants `DOS=HIGH`, `DOS=UMB`; the game needs none of it (PC98.md, M5.4 and M7), so the four lines are commented out *in the copy* (same length, `REM` in front; the baseline for "changed" is taken after the edit,
and an image saved from the copy keeps the `REM`s). `virtual_config=off` or `--no-virtual-config` leaves images alone.

**Keys** are mapped by position on a US keyboard (`keymap.c`): letters and digits as they are, arrows, Return, Esc, Space, Backspace, Tab, Delete, Insert, Page Up/Down (ROLL UP/DOWN), Home (HOME/CLR), End (HELP), the numeric keypad (the games read its 8, 4, 6, 2 as arrows too), F1 to F10, Shift (both) = SHIFT,
Ctrl (both) = CTRL, Alt (both) = GRPH, Scroll Lock = STOP. A US `[` types the JIS keyboard's `@` (the key in that place), `'` the `:` key, `=` the `^` key, `\` the yen key; CapsLock, the Windows keys, F11 and the grave accent are not mapped. Press and release go to the guest as they happen; the host's auto-repeat is dropped (the games poll the key status table; the PC-98 keyboard repeats by itself and the HLE BIOS does not,
so holding a key at the DOS prompt does not repeat). Focus lost, a menu opened, a reset: every key the guest believes down is released.

**F12** opens a menu that the keyboard drives (the PC-98 has no F12): Up/Down/Home/End, Enter, or the letter on the left of each row: **P** pause or resume, **R** reset, **H** reset from the hard disk, **1** **2** insert floppy 1/2, **3** open a hard disk image, **S** save changes as, **F** speed (real time, double, as fast as possible, half), **M** sound on or off, **+** **-** (or Left/Right) volume,
**V** size (1x, 2x, 3x, fit), **A** About, **Q** quit; Esc or F12 goes back. The machine is held while a menu or a dialog is open. The menu bar has the same things (and the idle skip switch) for the mouse.

**The window** is a menu bar, the picture and a status line; the picture is drawn at a whole-number scale (View: 1x, 2x, 3x, fit to the window; fit takes the largest whole number that fits, the rest is black) with square pixels. It opens at 1x (640 x 400): the program is a 1x window (`kapp_set_hidpi(false)`, like the Gallery and the games), so on a hi-dpi screen the window server shows every pixel as an N x N block and the picture is as sharp as it can be (tested at 2560 x 1440: bars exactly 160 pixels wide).
The status line says what the machine does (`56.4 fps  100% speed  (idle skip 41%)`, *Paused*, *Stopped*, *No disk in the drives*), the unsaved changes, and on the right which font draws the text (*built-in font: no kanji* without a ROM, *font ROM*), and *no sound device* when there is none.

**Settings** are `~/.config/pc98/config`, one `key=value` per line (`config.h` lists them): `font=` (the owner's `FONT.ROM`, 288,768 bytes, read at run time: without it the text of the DOS screens uses the library's built-in 8x16 font, ASCII only, no kanji, and the status line says so), `rhythm=` (a folder with the owner's `2608_BD.WAV`, `_SD`, `_TOP`, `_HH`, `_TOM`, `_RIM`: the YM2608's drums live in its ROM, so
without them the rhythm channel is silent), `cpu=` (8086, v30, 286, **386**, 486), `mhz=` (**16**), `fm=` (**86**, 26k, none), `sound=`, `volume=` (80), `scale=`, `speed=` (100, or max), `idle=`, `virtual_config=`. Wrong or unknown lines are skipped and reported on stderr; the command line (same words, `--font FILE`, ...) wins for that run and is not written back;
only settings changed from a menu are saved. Nothing from the owner's files is ever stored in the settings except their paths.

**Errors** are boxes in the application's own window set (nothing blocks): an image that is missing, damaged or not a disk image says which and why (`/tmp/x.d88: No such file or directory`, `the image is damaged (a header field points outside the file)`, `this is not a PC-98 disk image (D88, FDI, HDI, or a raw floppy dump ...)`), the other images still load; a guest that stops (an instruction the CPU model does not have, protected mode, a CPU shutdown, a boot sector that returns to the BIOS,
the same exception over and over, `CLI; HLT`) gets a box with what and where (`The program ran an instruction the emulator does not have: opcode 0F 07 at 1FC0:0000.`) and the window keeps the last picture and the menus; Reset starts again. A program that asks the BIOS to restart the machine is restarted (up to three times, and only if it ran at least a second since the last start). A broken image cannot hurt more than its own machine: the library checks every header field against the file size (and 600 damaged images run under AddressSanitizer in the host tests).

## How it works

| File | What |
|---|---|
| `keymap.c` | Fairies key code to PC-98 key number; `Keys` keeps the host's idea of what is down in step with what the guest was told (two Shift keys, a release whose press was never seen, release-all) |
| `config.c` | the settings file and the recent list: a forgiving parser that reports what it skipped |
| `pacer.c` | the machine's clock against the monotonic clock: clocks owed, a `lead` (the machine runs 50 ms ahead while there is sound, so the card never starves), forgiveness of a debt beyond 250 ms (slow motion instead of a burst), speed 50/100/200/max, the measured fps and speed. 64-bit integer arithmetic only (the program has no 128-bit division) |
| `audiopipe.c` | the FM chip's frames (55,466.67 Hz, the library's windowed-sinc resampler) and the buzzer (a square wave of `pc98_beep_hz()`, quiet) on the machine's timeline into a 48 kHz ring of a third of a second; volume on a square curve; an underrun is counted (the device plays silence), an overrun drops the oldest frames |
| `disks.c` | images in memory, block checksums, save-as with `O_EXCL`, the virtual CONFIG.SYS |
| `idle.c` | the idle skip (below) |
| `emu.c` | the machine without a window: library machine, disks, font (a `FONT.ROM`, or any dump with the 8x16 table at 800h, or the built-in font), drum samples, the pacer, the idle skip, the sound pipe, keys, the stop messages; `emu_step()` runs the machine for the time that is owed in 256-clock slices (the library's own), feeds the pipe, and draws the picture **only at the last frame boundary it owes** (a machine that is behind runs several frames and shows the newest: emulation is never skipped, pictures are) |
| `main.c`, `ui.c`, `dialogs.c`, `sound.c` | the desktop program: command line and settings files, the benchmark; one frame of the window; the dialogs; the sound pump (`/dev/audio` opened non-blocking, written only what the device has room for and never more than 160 ms ahead; no device: no sound and no error; faster or slower than real time: silent) |

**Size** ([UPDATE.md](UPDATE.md), "Room on the ESP"): the stripped program is 394,104 bytes, the second largest of the programs after `kappabox` (902,280) and a little more than the Explorer (390,632). It holds the whole emulator library and no assets: the font, the drum samples and the games are read at run time.

The host tests compile `keymap`, `config`, `pacer`, `audiopipe`, `disks`, `idle` and `emu` as they are, with libpc98, into `tests/host/pc98apptool.c`: the machine, the disks and the idle skip are tested without a desktop, on a fake clock, with the code the program runs.

### The idle skip (`idle.c`)

The games wait for the next frame in a busy loop: TH01 and TH04 both use `mov ax,[counter]; cmp ax,[bp+6]; jc again` (the VSYNC interrupt's handler counts the counter up), 63 % of the clocks of a gameplay frame (PC98.md, M7). The skip finds such a loop and moves the machine's clock to the next thing that can change what the loop sees. It is exact in the sense that matters: the guest cannot tell.
Before every skip it **checks**, with the CPU's bus wrapped for a probe of 128 to 512 clocks: (1) the state of the CPU (all registers, flags, IP) before some instruction of the probe equals the state at its end, so the stretch between is a closed loop of C clocks and n instructions; (2) in that stretch no interrupt was taken, no `INT` ran, no port was written, display memory was not read,
the only ports read are the keyboard status and the display status byte, and every RAM write nets to zero (a `push` of the same value to the same place); (3) the **horizon**: the clocks to the next frame boundary, the PIT counter 0's next edge (a lower bound from its count) *when IRQ 0 is unmasked*, the FM board's next interrupt (`opna_ticks_to_irq`) *when its line is unmasked*, a key waiting to be delivered, the VSYNC interrupt *when armed and unmasked*,
and, when the loop reads the display status, the next VSYNC or HBLANK edge; sources whose interrupt cannot fire (masked, or IF clear) do not count: TH01's start-up runs the timer free with a count of 3 and nobody listening; counting those as events made the skip a loss, found by profiling TH01 and fixed with a test. A pending interrupt request (IF set) forbids any skip: it is taken at the very next instruction.
The skip is a whole number of iterations short of the horizon by a margin of 128 clocks plus one period: the CPU is left in the state it would have, `cpu.cycles` and `cpu.instructions` add up, and the devices are advanced by exactly that many clocks with `pc98_advance()`. The caller never loses control: it passes the machine time it may move and gets back what passed.
After an interrupt handler returns into a loop seen before, the next probe comes at once (the loop's address range is remembered) and is just long enough (4 periods + 64 clocks). A probe that finds nothing backs off (1, 2, 4 ... 64 stretches) so that real work is not probed over and over.

*What it does not skip*, by test: a loop that counts in a register, writes memory that stays changed, writes a port (even one it may read), calls the BIOS (`INT`: the HLE BIOS's work is invisible to the wrapper), or reads display memory. The status-port polls (`in al,0A0h; test al,20h; jz`) are skipped only up to the next HBLANK edge (a scan line, about 640 clocks): 20 to 30 % of such a loop. A refinement that would learn which status bits a loop looks at
(perturb the value, see whether the path changes) would take the whole VSYNC wait; it was not needed for the games measured (their frame waits are the memory kind).

**What it saves** (host, WSL, gcc -O2, 386 at 16 MHz, FM board, the owner's images, deterministic counts; the first 44 to 56 s of machine time of each game, the boot, logo and title included, which are not idle):

| Game | Clocks skipped | Instructions not executed | Notes |
|---|---|---|---|
| TH01 (to stage 1) | 48 % | 41 % | in stage 1 alone (450 to 700 M clocks) 62 % of the clocks |
| TH02 | 58 % | 50 % | |
| TH03 | 14 % | 11 % | the run was mostly loading and menus |
| TH04 | 21 % | 14 % | the weapon selection waits in the memory loop; PMD86's timer ticks every 16,000 clocks so stretches are short |
| TH05 | 28 % | 19 % | |

Time on the host varies with the PC's load (two runs of the same thing differed by a factor of two while other jobs ran); one quiet run of TH01 to 700 M clocks took 6.78 s of CPU without the skip (6.5 times real time, FM synthesis included) and 4.32 s with it (10.1 times): the part that is not interpretation is the FM synthesis (about a quarter),
the picture (820 us per frame) and the probes. The synthetic frame wait of the tests (97 % idle) runs **30 times real time with the skip against 0.6 times without it** in the QEMU guest (TCG), see below.

## Speed

`pc98 --bench SECONDS [--bench-key S:HEX]... IMAGE` runs the machine headless, flat out, for SECONDS of machine time twice, the skip off and then on, and prints the speed in times real time (with sound synthesised and thrown away unless `--no-sound`; keys are PC-98 key numbers pressed S seconds into the machine's time, held about 80 ms).
Measured in the QEMU guest with the test's synthetic frame-wait image (`tests/case_pc98app.py`, `pc98app_benchmark_...`, the host PC loaded):

```
pc98app: bench: 80386 16 MHz, FM board, sound off
pc98app: bench idle=off: 2.00 s of machine time in 3.51 s of host time = 0.6 x real time (112 frames)
pc98app: bench idle=on : 2.00 s of machine time in 0.07 s of host time = 30.3 x real time (112 frames, the idle skip took 99% of the clocks ...)
```

Real games on the owner's disks cannot be run in QEMU by the tests (the files are the owner's and are not in the tree); to measure on the Dell, copy the images and `FONT.ROM` to `/HumanVillage/pc98/` and run for example (TH01: the title takes Return twice, Z throws the orb; times are the machine's, from M5/M7 of PC98.md: Return at 15.6 s and 18.8 s, Z at 26.3 s):

```
pc98 --font /HumanVillage/pc98/FONT.ROM --bench 40 --bench-key 15.6:1c --bench-key 18.8:1c --bench-key 26.3:29 /HumanVillage/pc98/th01.hdi
```

It prints one line per pass; "x real time" is what the Dell does; the application needs 1.0 (and, interactively, a margin for the desktop). On the development PC (WSL) the five games run 5 to 14 times real time without the skip. Interactive use (the Dell, QEMU) is paced by the monotonic clock whatever the speed.

## Tests

Host (no QEMU): `python tools/hosttest.py discover -s tests/host -p "test_pc98app*.py" -v` (50 tests, about 40 s; AddressSanitizer and UBSan). One test per glue function (the key table, the held-key bookkeeping, the config parser with bad lines, the recent list, the pacer on a fake clock: real time, speeds, a speed change, a long stall, the per-pass cap, the lead, the measurement; the sound pipe: timeline, buzzer pitch,
overrun drops the oldest, underrun counted, volume curve, the FM stream resampled with pitch and level kept; the disk module: every format, errors as sentences, the overlay never reaching the file, save-as always to a new file, 600 damaged images, the virtual CONFIG.SYS; WAV parsing, the drum folder, the font), then ten guest programs
(`tests/host/pc98app/idle.asm`) run with the skip off and on (the guest's counters, and where its loop was at each of 64 timer interrupts, must be identical; the skip must skip the games' wait, a wait with a call in it, a masked-timer start-up, and not a counting loop, a writing loop, a port-writing loop, even on a port it may read) and the machine as the program runs it (every image format boots, a guest write goes to the copy and not the file, "save as", pictures not emulation are skipped when behind, what stops the machine is reported with what and where, the buzzer and the FM board come out of the sound pipe at the right pitch).
Mutation-checked by hand: dropping the net-zero rule, the port-write rule, the masked-source rule, or the pending-interrupt rule each fails a named test (the last one failed first by itself: the timer ticks came late and in pairs).

QEMU: `python tools/qemu-slot.py -- python tests/run_all.py pc98app` (15 tests, about 16 minutes; `TOUHOU_TIMEOUT_SCALE=3` on a loaded PC). Synthetic floppy images are typed into the guest over the console (base64) and the program is started with `--trace`, whose lines the tests wait for (the guest clock can be slow under TCG; typing an image is the slow part, the guest's line editor redraws the line after every character, and a busy host once made a 30 s wait fail a group run, so `put_file` waits up to 180 s per line). The demo boot sector `tests/host/pc98app/appdemo.asm` draws eight colour bars, text, a box that follows the Z key,
a frame counter, a 2 kHz beep and a disk write on W. Checked in the window's pixels: the bars, the text and the box exactly (1x, 2x at 1920 x 1080, the server's 2x at 2560 x 1440); a key held down and released through the window; F12, P, P, R (the counter stops, goes on, starts over); a guest write that leaves the file alone, a refused overwrite, a save to a new file that differs only in that sector; the quit question (the machine is held behind it,
Esc keeps running, "Discard and quit" ends the program); the file dialog and the recent list; the font ROM named in the settings file (a made-up ROM of solid glyphs: exactly eleven blocks in the banner); the start menu entry and the desktop icon; a program that stops; the About box; missing, damaged and not-an-image files in one message; the buzzer and the FM board's SSG in the sound card's capture (2 kHz, 1006.5 Hz); the benchmark.

## Not done, approximated, unverified

- **Not run on the Dell** (nobody did): the sound path on the HDA codec (QEMU's `hda-output` works), the speed there, the hi-dpi look on the owner's monitor. Everything was checked in QEMU and in the host harness.
- Mouse and joystick: the PC-98 bus mouse (ports 7FD9h-7FDFh) and the joystick bits of the FM board's SSG port are not emulated.
- The keyboard: no auto-repeat in the guest (the HLE BIOS has none), the symbol keys are by position (a US `:` is Shift+`;`, which types `+` on the JIS layout), CapsLock and KANA (locking keys) are not mapped, no key remapping.
- The drums of the YM2608 need the owner's `2608_*.WAV` (`rhythm=`); the PCM drum parts of PMD86 are silent (PC98_SOUND.md); nobody has listened to the FM mix through the application (the levels are the library's).
- The idle skip is exact for pure memory loops and conservative for the rest: loops that call the BIOS, and status-port polls beyond the next HBLANK edge, run as they are. A memoised loop (record its states and the memory it reads, skip without a probe when they match) would save the probes (a quarter of the idle time in TH01); not written.
- The "fit" size is checked by logic only (no QEMU test resizes a window); 1x, 2x and the server's 2x are checked in pixels.
- The pictures are drawn at the machine's last frame boundary; a game that draws into the displayed page shows tearing as the real monitor did.
- CPU, clock and FM board changes are settings, not menu items (a restart applies them): `cpu=`, `mhz=`, `fm=`.

## How to resume

`git switch agent/pc98-app`; `python tools/hosttest.py discover -s tests/host -p "test_pc98*.py"` (the library's suites and this one); `python tools/build.py`; `python tools/qemu-slot.py -- python tests/run_all.py pc98app`. To look at a game on the development PC:
`python tests/host/pc98apphost.py run --disk "<image>" --font "<FONT.ROM>" --cycles 700 --key 250:1c:60 --png build/pc98/x.png` (the pictures stay under `build/`, git-ignored; the options are in the header of `tests/host/pc98apptool.c`). Next steps in order of value: the memoised idle loop, a menu for CPU/clock/FM, the PC-98 mouse, the symbol keys by character.

**Changes outside `user/bin/pc98app`, `tests/` and this file:** `user/bin/hakurei-shell/launch.c` (one line: the start-menu entry), `user/bin/hakurei-shell/desktop.c` (one line: the desktop icon), `tools/build.py` (one line: `bin/pc98` is a link to `pc98app`); `docs/STATUS.md` (a row). Nothing in `user/lib/pc98` was changed.
