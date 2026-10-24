# Touhou theming guide

TouhouOS should feel like a strange, legitimate operating system from Gensokyo, not a normal OS with a wallpaper.
Rule of thumb: **the joke must sit on top of the real mechanism** (a "Boundary violation" really is a page fault) and the
plain technical fact must stay readable next to it. Module and file names stay descriptive; the theme lives in log tags,
user-visible text, default content, assets and the desktop.

All art is original or procedurally generated. No assets are extracted from the games (see README fan-work notice).

## Subsystem tags (`klog("Tag", ...)`)

| Tag | Subsystem | Reason |
|-----|-----------|--------|
| Gensokyo | kernel core, boot | the kernel is "Gensokyo" |
| Boundary | memory management, paging | Yukari's boundaries = address-space boundaries |
| Sakuya | timers, clock, tick | time manipulation |
| Alice | tasks, processes, scheduler | she moves puppets on strings |
| Eiki | faults, permissions, security | the judge: incidents are reported to Eiki |
| Kappa / Nitori | hardware, framebuffer, window toolkit, sound (the mixer, `/dev/audio`, the HDA and AC'97 drivers) | kappa engineers; the music player is Reimu's Jukebox |
| Marisa | PCI and block devices (she collects everything on the bus) | borrows without asking |
| Hakurei | init and the login screen | the shrine opens for the day |
| Satori | keyboard | she reads your mind (your keystrokes) |
| Koishi | mouse | unconscious movement |
| Keine | filesystems, history | keeps history |
| Yukari | gaps = pipes, IPC, networking (the stack, the cards, DHCP) | gaps in the boundary; the packet-pushing kernel thread is "Yukari" too |
| Yuyuko | out of memory, trash | hungry; the Netherworld takes deleted files |
| Fairies | compositor / window server | three fairies of light hold the pixels |
| Patchouli | documentation (`patchouli` command, man pages) | the library |
| Aya | Bunbunmaru News, screenshots (`[aya] Aya took a photo of the screen`) | the reporter and her camera |

## Versions

Kernel releases are named after Touhou game titles in release order: 0.1 *Story of Eastern Wonderland*,
0.2 *Phantasmagoria of Dim.Dream*, 0.3 *Lotus Land Story*, 0.4 *Mystic Square*, 0.5 *Embodiment of Scarlet Devil*,
0.6 *Perfect Cherry Blossom*, 0.7 *Imperishable Night*, 0.8 *Phantasmagoria of Flower View*, 0.9 *Mountain of Faith*,
1.0 *Subterranean Animism*.

## Filesystem

Thematic directories are real; conventional POSIX names are symlinks to them.

| Path | Meaning | Conventional alias |
|------|---------|--------------------|
| `/Hakurei` | system configuration and shrine records | `/etc` -> `/Hakurei/etc` |
| `/HumanVillage` | user home directories | `/home` |
| `/ScarletDevilMansion` | installed applications, libraries, docs (Patchouli's library) | `/usr` |
| `/YoukaiMountain` | devices and news (Kappa workshop, Bunbunmaru) | |
| `/BambooForest` | temporary files (you get lost in there) | `/tmp` |
| `/Netherworld` | trash: `trash` and the Explorer's Delete send files here; `rm` deletes for good | |
| `/.boundary` | hidden system metadata | |
| `/bin`, `/dev`, `/proc`, `/sys` | as usual | |

## People

Users: `root` (GECOS "Yukari Yakumo", locked), `reimu` (1000, the default resident), `marisa` (1001, in the sudoers file: she
borrows everything anyway), `sakuya` (1002), `sumireko` (1003, an outsider with no seat at the shrine, so no desktop and no power
button). Group `shrine` (100) is the seat: display, input devices and the power button. Hostname: `gensokyo`.
Prompt: `reimu@gensokyo ~ $`. See [ACCOUNTS.md](ACCOUNTS.md).

## Messages

- Page fault in kernel: `BOUNDARY VIOLATION: ...`; any fatal kernel error: `*** INCIDENT: KERNEL PANIC ***` plus a quip that
  matches the fault (null pointer, stack overflow, out of memory, ...). The register dump and backtrace always follow.
- `sudo` (it is called `eiki`) for someone not in `/etc/sudoers`: `reimu is not in the sudoers file. This incident will be reported to Eiki.`
  and a line `[Eiki] reimu tried to borrow root's power and was refused` in the kernel log.
- A wrong password at the login screen: the card shakes its head, "Login incorrect / Eiki has made a note of it."
- `init` logs in the voice of the shrine: `the shrine opens for the day`, `reimu left the shrine; the gate closes behind them`.
- The window server's last resort (Ctrl+Alt+Backspace) opens the text door instead of handing out a shell.
- Out of memory: `Yuyuko is hungry.`
- USB sticks are carried by Nitori's kappa (hardware, tag `Kappa`): `usb-storage 1-2: ... the kappa are sizing the stick up`, `... carried
  over the river as sdb`, a write-protected stick is "the kappa keep the sluice gate shut (read-only)"; the volume belongs to Keine
  (`the Human Village keeps its records on the stick now`, `... vanished while it was mounted`); the kernel log on the stick is
  "Shikieiki's ledger, readable on any other computer". The block layer's own lines (`blk: sdb removed ...`) stay Marisa's.
- The compiler speaks like Marisa, but the technical fact comes first and stays conventional: `mcc` prints `file:line: error: text`,
  the source line, a caret and a remark (`^ Marisa: I never borrowed that one, ze. Did you declare it?`), and ends with
  `mcc: 1 error -- nothing was built, ze.`; `mld` reports `undefined reference to 'x' -- Marisa: that book isn't in the library, ze`;
  a successful build prints nothing. `mcc --version` says who she is.
- `trash` -> Netherworld (`rm` deletes for good), `poweroff` -> "The shrine maiden is closing up for the night. Goodbye!", `fortune`/`omikuji` -> shrine fortune slips, `saisen` -> offering box.
- The Explorer: Delete sends things to the Netherworld (with a note of where they came from), an empty Netherworld says `Yuyuko is
  hungry.`, deleting for good or emptying asks whether Yuyuko may eat it, an empty folder shows `Nothing here but the wind`, and
  the permission errors keep their Eiki flavour from `strerror`. The plain fact (what failed, which path) is always in the text.

## Desktop (HakureiDE)

Late-90s / PC-98 flavoured, restrained: shrine red (`#C8102E`) and white, deep night-blue backgrounds, procedurally drawn
yin-yang, torii, moon and bamboo. Applications: Gensokyo Explorer (files), gapsh Terminal, Spell Manager (processes as
"spell cards"), Bunbunmaru News, Kappa Control Panel (settings/hardware), Patchouli's Notebook (editor), Ran's Calculator,
Sakuya's Pocket Watch (clock), Reimu's Jukebox (the Prismriver Concert Hall: Lunasa, Merlin and Lyrica play on a stage), a danmaku shooter, and small games.
