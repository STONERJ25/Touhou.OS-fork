# TouhouOS user guide

This page is for someone sitting in front of the machine: what is on the screen, how to start things, which keys do what, where
your files go and how to switch the machine off. It describes what is on `main`. Features that are finished but still sit on unmerged
`parked/*` branches are listed at the end under [Not merged yet](#not-merged-yet).

Everything on the system has a manual page: open a terminal and type `man NAME` (`man` is Patchouli's library, `patchouli`). Good first
visits are `man gensokyo` (the lay of the land), `man keys`, `man gapsh`, `man accounts` and `man power`. `patchouli -l` lists every page.

A word on names: the system is dressed as Gensokyo, the land of the Touhou Project. The costume always sits on top of a plain
mechanism, and this guide gives both: the "Netherworld" is the trash folder, a "spell" in the Spell Manager is a process, and so on.

## Contents

1. [Starting up](#starting-up)
2. [The desktop](#the-desktop)
3. [The applications](#the-applications)
4. [Keyboard and mouse](#keyboard-and-mouse)
5. [The terminal and the shell](#the-terminal-and-the-shell)
6. [Files and where they live](#files-and-where-they-live)
7. [Accounts, logging in and locking the screen](#accounts-logging-in-and-locking-the-screen)
8. [Shutting down and restarting](#shutting-down-and-restarting)
9. [Sound, network, pictures](#sound-network-pictures)
10. [Boot options](#boot-options)
11. [Limits and known problems](#limits-and-known-problems)
12. [Not merged yet](#not-merged-yet)

## Starting up

The firmware starts the loader (`EFI/BOOT/BOOTX64.EFI` on the boot stick), the loader reads `cmdline.txt`, `kernel.elf` and `initrd.img` from
the same partition, and the kernel ("Gensokyo") prints its log on the screen while it finds the hardware. Then `init`, the shrine maiden, opens
the door:

- By default the **desktop opens for `reimu`** at once, with no password.
- With `login` in `cmdline.txt` the **login screen** asks who is there (see [Accounts](#accounts-logging-in-and-locking-the-screen)).
- With `text` (or `nogui`) in `cmdline.txt`, or on a machine without a usable screen, you get a **text console** with a shell instead of the desktop.

How to make a stick and which firmware settings a PC needs is in [REAL_HARDWARE.md](REAL_HARDWARE.md). Running it in QEMU is in the
[README](../README.md).

## The desktop

The desktop (HakureiDE) is a wallpaper with a few icons, a panel along the bottom edge, and the windows of the programs you start.

**The panel**, from left to right:

| Part | What it does |
|------|--------------|
| **Gensokyo** button | opens the start menu (a tap of the Super/Windows key does the same) |
| three small buttons | quick launch: Terminal, Explorer (your home folder), Notebook |
| task list | one button per open window: click to bring it to the front, click the active one to minimise it, **middle-click to close it** |
| two small bars | CPU use (yellow) and memory use (blue); hover for the numbers |
| clock | hours and minutes; hover for the date, click for a month calendar (arrows change the month) |
| narrow button at the right edge | show the desktop: minimises every window, a second click brings them back |

**The start menu** has the sections *Programs*, *Games*, *System* and *Session* (Lock screen, Shut down...). An application only appears in it when
its program is installed in `/bin`.

**Desktop icons** (double-click to open): *Home* (your home folder in the Explorer), *Netherworld* (the trash), *Terminal*, *Explorer* (the
whole file system, from `/`), *Notebook*, *Spell Manager*.

**Right-click on the desktop** for a menu: Open Terminal, Gensokyo Explorer, Take screenshot, About TouhouOS (version, processor and memory),
Lock screen, Shut down...

**Windows** have a title bar with three buttons (minimise, maximise, close). Drag the title bar to move a window, drag an edge or a
corner to resize it, double-click the title bar of a resizable window to maximise or restore it, or hold **Alt** and drag anywhere inside the window to move it.
The Notebook and the Sketchbook mark unsaved work with `*` in the title and ask before they close.

Programs started from the panel or the menu write their messages to `/tmp/<program>.log` (for example `/tmp/explorer.log`), which is the
first place to look when something closes by itself.

## The applications

| In the menu | Command | What it is |
|-------------|---------|------------|
| Terminal | `gapterm` | a terminal window running the shell, `gapsh` |
| Gensokyo Explorer | `explorer [PATH]` | file manager |
| Patchouli's Notebook | `notebook [FILE]` | text editor |
| Ran's Calculator | `calculator` | calculator with a scientific keypad and a history tape |
| Sakuya's Pocket Watch | `pocketwatch` | clock, stopwatch, countdown timer and calendar |
| Bunbunmaru News | `bunbunmaru` | a daily newspaper: gossip, puzzles and real news about the machine (uptime, memory, disks, the kernel log) |
| Marisa's Sketchbook | `sketchbook [FILE]` | paint program (BMP and PPM files) |
| Gensokyo Gallery | `gallery [FILE or FOLDER]` | picture viewer (PNG, JPEG, GIF, BMP, PNM, TGA) |
| Spell Manager *(System)* | `spellmgr` | task manager: processes ("spells"), performance graphs, hardware, users |
| Kappa Control Panel *(System)* | `kappa-panel` | settings and hardware information |
| Danmaku *(Games)* | `danmaku` | a vertical bullet-hell shooter |
| Minesweeper *(Games)* | `minesweeper` | Minesweeper, with fairies under the grass instead of mines |
| Snake *(Games)* | `snake` | Snake: eat the dango |

The window titles say plainly what a window is ("Terminal", "Task Manager", "Notebook - notes.txt"); the Touhou names are in the menu.
Most applications have a menu bar, F1 for help or the list of keys, and Ctrl+Q (or Ctrl+W) to close. The details of each program are in
[APPS.md](APPS.md), [APPS_notebook.md](APPS_notebook.md), [APPS_sketchbook.md](APPS_sketchbook.md), [EXPLORER.md](EXPLORER.md),
[GAMES.md](GAMES.md) and [MEDIA.md](MEDIA.md); the short version:

- **Gensokyo Explorer**: list or icon view, sort by clicking a column heading, places on the left (Home, the root, the Netherworld, mounted
  volumes). Double-click or Enter opens: folders are entered, text files open in the Notebook, pictures in the Gallery, programs run in a
  terminal window. Delete sends to the Netherworld, Shift+Delete deletes for good (it asks first), F2 renames, Ctrl+C / Ctrl+X / Ctrl+V copy,
  cut and paste files, Ctrl+H shows hidden files, F5 reads the folder again, Alt+Enter shows properties (permissions, owner). Files can be
  dragged onto a folder or a place. Long copies show a progress window with Cancel.
- **Patchouli's Notebook**: Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S (new, open, save, save as), Ctrl+Z / Ctrl+Y undo and redo, Ctrl+F find,
  Ctrl+H replace, Ctrl+G go to line, F3 next match. Colours for C, shell scripts and Markdown. Closing a page with unsaved changes asks
  Save / Don't save / Cancel.
- **Ran's Calculator**: type an expression (`2pi`, `sqrt(2)`, `200+10%`) and press Enter. Alt+1 / Alt+2 switch between the standard and the
  scientific keypad, F3 / F4 between degrees and radians. Ctrl+C copies the result.
- **Sakuya's Pocket Watch**: Ctrl+1..4 switch between Clock, Stopwatch, Timer and Calendar. In the stopwatch Space starts and stops, L takes a lap,
  R resets. T stops time on the clock face (press T again to let it run). The clock follows the 12/24 hour setting of the Control Panel.
- **Spell Manager** (also **Ctrl+Alt+Delete**): Up / Down choose a process, Delete ends it (SIGTERM, "End Spell"), Shift+Delete kills it
  (SIGKILL, "Exorcise"); both ask first. Ctrl+F filters, F5 refreshes, Ctrl+1..4 switch tabs.
- **Kappa Control Panel**: panes for System, Display, Keyboard & Mouse (shows every key and button you press, handy for testing a keyboard),
  Devices, Users (with *Change password...*), Power (Shut down, Restart) and Appearance (12/24 hour clock, the wallpaper). The screen
  resolution is not changed here: it is chosen at boot (`video=` in [Boot options](#boot-options)).
- **Gensokyo Gallery**: Left / Right (or Space / Backspace) for the previous and next picture of the folder, the wheel or `+` / `-` to zoom,
  `0` or F to fit, `1`..`4` for 100% to 400%, drag to pan, R / L rotate, H / V flip (the file is never changed), S or F5 slideshow,
  **W makes the picture the wallpaper**, I shows details, Ctrl+O opens another file. Without a file it opens the newest picture in `~/Pictures`.
- **Marisa's Sketchbook**: tools on the left (keys P pencil, B brush, E eraser, A spray, L line, R rectangle, O ellipse, F fill, I colour
  picker, T text, S select); the left button paints with the foreground colour, the right button with the background colour; `[` and `]` change
  the size. It opens and saves 24-bit BMP and PPM pictures, not PNG.
- **Danmaku**: arrow keys move, Z shoots, X bombs, Shift slows you down and shows your hitbox, Esc pauses.
- **Minesweeper**: left click uncovers, right click places an ofuda (a flag), middle click (or both buttons) uncovers around a number; F2 starts a
  new game. The arrow keys, Space, F and Enter play it from the keyboard.
- **Snake**: arrow keys or WASD steer, Space / P / Esc pause.

High scores and settings are kept in `~/.config/` (for example `~/.config/danmaku.scores`, `~/.config/hakurei.conf`).

## Keyboard and mouse

The keyboard layout is US English; there is no other layout yet.

**Anywhere on the desktop** (handled by the window server, Fairies, and the desktop shell):

| Keys | What happens |
|------|--------------|
| Super (tap it alone) | open or close the start menu |
| Ctrl+Alt+T | a new terminal window |
| Ctrl+Alt+Delete | the Spell Manager (task manager) |
| Alt+Tab, Alt+Shift+Tab | switch between windows (keep Alt held and press Tab again to go further; let go of Alt to choose) |
| Alt+F4 | close the active window (it asks first if it has unsaved work) |
| Super+L | lock the screen |
| Ctrl+F12 | screenshot of the whole screen, saved as `~/Pictures/Screenshot-YYYY-MM-DD-HHMMSS.png` |
| the machine's power button | open the *Leave the shrine?* dialog; a second press powers off |
| Ctrl+Alt+Backspace | emergency exit: the kernel kills the window server (use it when the desktop hangs). You land at the text login prompt, and the login screen comes back after that |

While the screen is locked none of these work except Ctrl+Alt+Backspace (which takes you to a login prompt, not past the lock).

**Text fields and text areas** in every application: Ctrl+A selects all, Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste (the clipboard is shared
between programs), Home / End, Shift with the arrows selects, Esc leaves a text field. Dialogs close with Esc.

**Mouse**: left button selects and drags, double-click opens, right button opens a context menu where there is one (the desktop, the Explorer,
the Notebook), the wheel scrolls the window under the pointer. Middle-click on a task button in the panel closes that window.

## The terminal and the shell

**The terminal** (`gapterm`, Ctrl+Alt+T) runs the shell, `gapsh`, as a login shell.

| Keys / mouse | What happens |
|--------------|--------------|
| select with the mouse | the selection is copied at once; double-click selects a word |
| right or middle click, Ctrl+Shift+V, Shift+Insert | paste |
| Ctrl+Shift+C | copy the selection |
| Ctrl+Shift+N | another terminal window |
| Shift+Page Up / Shift+Page Down, the wheel | scroll back through the last 3000 lines |

Ctrl+C, Ctrl+D and the other control keys go to the program running in the terminal, as on any Unix terminal (`man keys`).

**The shell** (`gapsh`, `man gapsh`) is a Bourne-style shell. The prompt looks like `reimu@gensokyo ~ $` (`#` for root, and the exit status of
the last command in front when it was not zero).

- **Line editing**: Left / Right, Home / End, Ctrl+A / Ctrl+E, Ctrl+K / Ctrl+U / Ctrl+W cut, Ctrl+Y pastes it back, Ctrl+L clears the screen,
  Ctrl+C abandons the line, Ctrl+D on an empty line leaves the shell.
- **History**: Up / Down walk through it, Ctrl+R searches it as you type; `history` lists it. The last 500 lines are saved in `~/.gapsh_history`.
- **Completion**: Tab completes a command name or a file name; a second Tab lists the choices.
- **Typos**: a command that does not exist gets a suggestion ("Did Yukari's gap lead you astray? Perhaps you meant 'ls'.").
- **Help**: `help` lists the builtins, `man NAME` shows a manual page (`q` leaves it, `/` searches), `man -k WORD` searches all pages,
  `kappabox --list` lists the about 140 small commands in `/bin` (`ls`, `cp`, `grep`, `sed`, `awk`, `find`, `tar`, `ps`, `top`, `df`, `ping` ...).
- **Ready-made aliases** (from `/etc/profile`): `ll`, `la`, `l`, `..`, `cls`, `shutdown` (= `poweroff`), `exorcise` (= `kill -9`),
  `spellcards` (= `ps`), `netherworld` (= `ls /Netherworld`), `desktop` and `startx` (start the desktop from a text console).
- **Your own settings**: `~/.gaprc` is read by every interactive shell.
- **Background jobs**: `command &` runs it in the background, `jobs` lists the running ones, `fg` waits for one in the foreground.
  **Do not press Ctrl+Z**: it stops the foreground program, but the shell does not take the prompt back yet, so the terminal freezes (close the
  window, or use the Spell Manager on the stopped program).
- **Editors in the terminal**: `pnote FILE` (Ctrl+S save, Ctrl+X exit, Ctrl+G help), `vi FILE` (a first version: the basics work).
  `less` / `more` page through text.
- Small pleasures: `omikuji` (today's fortune slip), `fortune`, `saisen` (the offering box), `reimusay`, `kappasay`.

**The text console** (no desktop, after `text` on the command line or after Ctrl+Alt+Backspace) is the same shell on the whole screen.
There is no scrollback there; pipe long output through `less` (`dmesg | less`). Typing `desktop` starts the desktop from it.

## Files and where they live

The directories are named after places in Gensokyo; the usual Unix names are links to them, so both work:

| Path | What it holds | Also known as |
|------|---------------|---------------|
| `/HumanVillage/NAME` | home folders (`reimu`, `marisa`, `sakuya`, `sumireko`) | `/home`, `~` |
| `/Hakurei/etc` | system settings: accounts, passwords, time zone, `profile`, `motd` | `/etc` |
| `/ScarletDevilMansion` | programs' data and the manual pages (`share/patchouli`) | `/usr` |
| `/BambooForest` | temporary files | `/tmp` |
| `/Netherworld` | the trash | |
| `/bin`, `/sbin` | the programs | |
| `/dev`, `/proc` | devices and live kernel information (`cat /proc/meminfo`, `cat /proc/mounts`, `cat /proc/usb`) | |
| `/mnt` | an empty folder for mounting another volume by hand | |

**What survives a restart.** Almost everything above lives in memory and is rebuilt from the boot image at every start, `/etc` included.
The one exception is the **HUMANVILLAGE volume**: a FAT32 partition (or disk) whose label is `HUMANVILLAGE`. The kernel looks for it at boot
and when a USB stick is plugged in, and mounts it read-write on `/HumanVillage`, so the home folders and everything in them are on that
volume and survive. Without one, the home folders are in memory too and are lost at power-off.

- **On a boot stick** made with `tools/mkimage.py` the big second partition is the HUMANVILLAGE volume, so your files are on the stick.
  It is ordinary FAT32: any Windows, macOS or Linux computer can read the files (the homes are the folders `reimu/`, `marisa/` ...).
- Only **one** volume is mounted by itself: the first HUMANVILLAGE volume found (one on an internal disk wins over one on a stick).
  Nothing else is ever mounted, formatted or written by itself. Another stick can be mounted by root by hand: `mount /dev/sdb1 /mnt`
  (see `man mount`, `lsblk` lists the disks).
- **Internal disks are read-only by default on a stick.** The stick's `cmdline.txt` contains `blk.ro`, which makes every SATA and NVMe disk
  read-only for the whole run, so not even root can write to the PC's own disks. (USB sticks follow their own write-protect switch instead.)
  This is the project's safety rule: the OS never writes to a disk it was not told to. On the owner's Dell the internal disks are not visible
  at all while the firmware's SATA mode is "RAID On".
- **Shut down before you pull the stick.** A clean shutdown marks the volume clean. If the stick is pulled, or the machine is switched off
  hard, within a few seconds of a write, the volume is still marked dirty and the next boot mounts it **read-only**, says so in the log, and
  writes nothing to it. The files are fine; a check on another computer clears the mark (Windows "Scan and fix", or `fsck.vfat -a` on Linux).
  Details in [REAL_HARDWARE.md](REAL_HARDWARE.md), section 5.
- **The kernel log** is kept on the volume as `TOUHOU.LOG` (this boot) and `TOUHOU.PRV` (the boot before), in its top folder, so a failed boot
  can be read on another computer. `dmesg` shows the same log on the machine.

**The Netherworld (trash).** The Explorer's Delete, and the command `trash FILE`, move things into `/Netherworld`; the Explorer can restore
them to where they came from. Note that `/Netherworld` lives in memory: what you throw away from your home folder is copied there and **is
gone after a restart**. `rm` deletes for good without the Netherworld.

**Where programs keep things in your home**: `~/Pictures` (screenshots), `~/.config/` (settings and high scores, the wallpaper choice in
`~/.config/hakurei/wallpaper`), `~/.gapsh_history`, `~/.gaprc`.

## Accounts, logging in and locking the screen

| Account | Who | Notes |
|---------|-----|-------|
| `reimu` (1000) | Reimu Hakurei | the default resident: her desktop opens at boot |
| `marisa` (1001) | Marisa Kirisame | may use `sudo` (she is in `/etc/sudoers`) |
| `sakuya` (1002) | Sakuya Izayoi | |
| `sumireko` (1003) | Sumireko Usami | an outsider: not in the group `shrine`, so no desktop and no power button |
| `root` (0) | "Yukari Yakumo" | locked: nobody logs in as root |

The accounts have **no passwords** to start with. Being in the group `shrine` (the "seat") is what allows someone to use the screen, keyboard
and mouse and to power off or restart the machine. Full details: [ACCOUNTS.md](ACCOUNTS.md), `man accounts`.

- **The login screen** (with `login` in `cmdline.txt`) shows a card for every resident with a seat: click one (or use Tab / the arrows),
  type the password (an account without one just needs Enter). A wrong password makes the card shake its head and is written to the log.
  Its buttons: *Text console* (a text login prompt once), *Shut down*, *Restart*.
- **Switching user**: *Shut down... -> Log out*. Logging out ends every program the resident left running and shows the login screen.
- **Locking**: Super+L, *Start -> Lock screen* or the desktop's right-click menu. Nothing can get in front of the lock; type the password
  (or just Enter for an account without one).
- **Passwords**: `passwd` (or the Control Panel's Users pane). Leaving the new password empty removes it.
- **Becoming someone else**: `su NAME` (asks for that resident's password if they have one). Root's account is locked, so `su` alone does
  not work: to run something as root, become `marisa` and use `sudo COMMAND`. Anyone not in `/etc/sudoers` is refused with "*reimu is not in
  the sudoers file. This incident will be reported to Eiki.*" and a line in the kernel log.
- **New residents**: `sudo adduser NAME` (and `sudo deluser NAME`).
- **None of this persists yet**: `/etc` is rebuilt from the boot image at every start, so new passwords and new accounts are forgotten at
  the next boot. Only the home folders on a HUMANVILLAGE volume survive.

## Shutting down and restarting

- **From the desktop**: *Gensokyo -> Shut down...* (or the right-click menu, or the machine's power button) opens *Leave the shrine?* with
  **Shut down**, **Restart**, **Log out** and **Cancel**. A second press of the power button while the dialog is open powers off.
- **From the Control Panel**: the Power pane (asks for confirmation).
- **From a shell**: `poweroff` (also `shutdown` and `halt`), `reboot` (or `shutdown -r`).
- **From the login screen**: its Shut down and Restart buttons; the power button powers off at once there. With no desktop running at all,
  one press of the power button powers off.

Every one of these flushes the files to disk first (and closes the kernel log on the HUMANVILLAGE volume with a "clean run" line). Only root
and members of the group `shrine` may power off or restart; anyone else gets *Operation not permitted* and the machine keeps running.
There is no suspend or hibernate.

## Sound, network, pictures

- **Sound** (Intel HDA and AC'97 sound cards): `volume` shows or sets the master volume (`volume 40`, `volume mute`); it starts at 60 at each
  boot. `beep` plays a tone, `play FILE.wav` plays a WAV file. There is no volume control in the panel yet.
- **Network**: when a network cable is plugged in, the kernel asks the network for an address by itself (DHCP). `ifconfig` or `ip addr` show
  the state, `ping`, `nslookup`, `fetch URL` / `wget URL` (plain `http://` only: there is no HTTPS on `main`), `sntp SERVER` shows how far off the
  clock is and `sudo sntp -s SERVER` sets it. There is no network panel; see [NETWORK.md](NETWORK.md) and `man network`.
- **Pictures**: screenshots with Ctrl+F12, *Take screenshot* in the desktop menu or `screenshot`; the wallpaper with the Gallery's W key, the
  Control Panel (Appearance) or `wallpaper PICTURE` (`wallpaper -c` goes back to the drawn night scene). `imginfo` and `convert` work on picture
  files from the shell.
- **Time**: the clock comes from the PC's clock chip. The time zone is set with `tz=` on the command line; if the time is hours off on a PC
  that also runs Windows, add `rtc=local` (see [POWER_AND_TIME.md](POWER_AND_TIME.md), `man clock`).

## Boot options

The kernel command line is the file `cmdline.txt` on the boot partition (the small 64 MiB FAT32 partition of the stick). Edit it on any
computer; the words are separated by spaces. A stick made with `tools/mkimage.py` contains `video=native blk.ro` unless you chose another line;
if the file is missing the loader uses the same two words.

| Word | Effect |
|------|--------|
| `video=native` / `video=max` / `video=1280x800` | screen mode: keep the firmware's (normally the monitor's own resolution) / the largest offered, up to 2560x1600 / exactly that one. Without `video=` the loader asks for 1024x768 |
| `blk.ro` | every SATA and NVMe disk is read-only (keep it) |
| `text` or `nogui` | no desktop, a text console |
| `login` | the login screen instead of reimu's desktop |
| `autologin=NAME` | open NAME's session instead of reimu's |
| `tz=ZONE` | the time zone in POSIX form, for example `tz=EST5EDT,M3.2.0,M11.1.0` (`man timezone`) |
| `rtc=local` | the PC's clock chip keeps local time |
| `nohumanvillage` | do not mount any HUMANVILLAGE volume |
| `bootlog=off` | do not write `TOUHOU.LOG` |
| `nonet`, `noaudio`, `usbstorage=off`, `usb=off` | leave the network cards, the sound card, USB sticks, or all of USB alone |

More words, for diagnosing a PC that does not boot, are in [REAL_HARDWARE.md](REAL_HARDWARE.md) (section 4) and [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md).

## Limits and known problems

- **Ctrl+Z in a terminal freezes it** (see [the shell](#the-terminal-and-the-shell)); the shell's job control is not finished.
- Settings in `/etc` (passwords, new accounts) are forgotten at every restart; only the HUMANVILLAGE volume keeps files.
- The screen is drawn at 1x: on a large monitor (2560x1440) the 8x16 text is small. There is no scaling yet.
- US keyboard layout only; no Japanese text.
- Only one processor core is used.
- Alt+F2 does nothing yet (the window server reserves it for a "run a command" dialog, which the desktop shell does not have).
- The clipboard holds at most 1024 bytes.
- The context menus of the applications (the Explorer, the Notebook) are drawn inside their window, so they cannot extend past its edges.
- The desktop panel's clock is always 24-hour; the 12/24 hour setting only applies to the Pocket Watch.
- A HUMANVILLAGE volume that was not shut down cleanly is mounted read-only until it is checked on another computer.
- On real hardware only the owner's Dell has been tried: its screen, USB keyboard and mouse, the boot stick and sound worked; its network card
  has not had a link yet, and its internal disks are hidden by the firmware's RAID mode.

## Not merged yet

These are finished (or nearly) on branches that are not part of `main` yet; they are listed so nobody looks for them on the desktop.
See [NEXT.md](NEXT.md) for their state.

| Branch | What it brings |
|--------|----------------|
| `parked/audio-jukebox` | Reimu's Jukebox (a music player) with original songs, a `.mod` player, system sounds, a volume slider in the panel |
| `parked/network-https` | HTTPS in `fetch` and `wget` |
| `parked/usb-dirty-fsck` | a stick that was not shut down cleanly is checked at boot and stays writable when it is consistent; an `fsck` tool |
| `parked/utilities-vi` | a much more thorough `vi` |
| `parked/libc-accuracy` | exact number printing and parsing, faster `sort` and `grep` on big files |
| `parked/kernel-hardening` | `rm *` and `xargs` on big folders (larger argument limits), fixes for crashes an ordinary user could cause |
| `parked/mcc-compiler` | a C compiler that runs on the OS |
