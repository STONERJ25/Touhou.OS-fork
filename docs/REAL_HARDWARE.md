# Running TouhouOS on a real PC

**Most of this has only been run in QEMU; one real PC, the Dell below, has booted it to the desktop from a stick since 2026-10-23**
([HARDWARE_AUDIT.md](HARDWARE_AUDIT.md) has what that showed). The drivers follow the specifications and Linux's behaviour, and they are
written to fail soft (log one line, stay out of the way), but a first boot on other hardware is an experiment. The
design rule that makes it a safe one: **the OS never writes to a disk you did not ask it to**, and the default command
line makes every SATA and NVMe disk read-only on top of that.

The machine this was prepared for is a Dell desktop with an Intel Core i5-7500 (Kaby Lake, 200-series chipset), an
Intel HD 630 driven through UEFI GOP, a USB keyboard and mouse (there is no PS/2 port to count on), and a SATA or NVMe
disk it never touches.

## 1. Build the stick

```powershell
python tools/mkimage.py --zip              # builds everything, writes build/touhou-os.img (1 GiB) and build/touhou-os.img.zip (7 MB)
```

The image is a GPT disk: a 64 MiB FAT32 *EFI System Partition* (`EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `initrd.img`,
`cmdline.txt`) and a FAT32 partition labelled `HUMANVILLAGE` for the rest. Options: `--size 4G`, `--out PATH`,
`--cmdline "..."` (see section 4), `--no-build`.

Write it to a USB stick (everything on the stick is replaced; double-check the drive letter):

- **Rufus**: pick the stick, "Select" the `.img` (or the `.zip`) file, and when asked choose **DD Image mode**.
- **balenaEtcher**: "Flash from file", pick the `.img` or the `.zip`, then the stick.
- **Linux/macOS**: `sudo dd if=touhou-os.img of=/dev/sdX bs=4M status=progress` (and `sync`).

Disk tools may offer to "repair" the backup GPT header because the image is smaller than the stick; that is harmless.

A smaller alternative for trying it out is `python tools/mkiso.py`: one ISO file (about 18 MiB) that boots as a virtual CD and, written the same way to a stick, as a disk, with no `HUMANVILLAGE`
volume (nothing is saved); see [ISO.md](ISO.md).

## 2. Firmware settings (Dell UEFI setup, F2 at power-on)

| Setting | Value | Why |
|---------|-------|-----|
| Boot mode | UEFI (not Legacy) | the stick boots through a UEFI loader |
| Secure Boot | **Disabled** | the loader is not signed |
| SATA operation | **AHCI** (not "RAID On" / Intel RST) | in RAID mode the internal disks are invisible to any OS without Intel's RST driver; TouhouOS prints a hint and leaves them alone, which is also fine if you only want to test the desktop. Do not switch an installed Windows to AHCI without preparing it first (it can then fail to boot) |
| USB boot / legacy USB | enabled | to boot from the stick (F12 opens the one-time boot menu: choose the entry that says UEFI and the stick's name) |
| Fast Boot | Thorough / minimal | otherwise the USB ports may not be initialised in time to show up in the boot menu |

Use rear USB ports directly on the board rather than hubs for the first try.

## 3. What you should see

1. The firmware's boot menu entry loads `BOOTX64.EFI`; the loader prints a few lines (`loader: framebuffer 1920x1080 ...`).
2. The kernel log scrolls by on the framebuffer console (there is no serial port to count on): `[Gensokyo] acpi: ...`,
   `[Marisa] pci ...` for every device found, drivers claiming devices, `[Keine]` filesystem lines, and `boot complete`.
3. About a second after the USB controller comes up the stick itself appears: `[Kappa] usb-storage 1-3: ... bulk-only transport`, then
   `[Marisa] blk: sda ...` with its two partitions, `[Kappa] usb-storage ... carried over the river as sda`, `[Keine] fat32: sda2 mounted on
   /HumanVillage` and `the Human Village keeps its records on the stick now`. The homes (`/HumanVillage/reimu` ...) are on the stick from
   here on and survive the next boot. Boot waits for this (at most 8 s, `[Kappa] usb: the bus settled after N ms`).
4. `init` opens the desktop for `reimu` (no password), the HakureiDE desktop with a panel. Press **Ctrl+Alt+T** for a
   terminal, **Super+L** to lock, the power button for the shutdown dialog. **Shut down from the panel (or `poweroff`) before you pull the
   stick**: that is what marks the volume clean. A stick that was just pulled (or a power button held down) is gone through before it is
   mounted at the next boot: usually it comes up read-write at once, see "What touches your disks" below.

If something is wrong, the useful things to look at (in a terminal window or the text console, `Ctrl+Alt+Backspace` leaves the desktop):

```
dmesg | less           the whole boot log        cat /proc/pci        every PCI device and who claimed it
cat /proc/acpi         what ACPI told us          cat /proc/usb        the USB device tree (keyboard, mouse, hubs, sticks)
cat /proc/partitions   disks the kernel sees      cat /proc/ahci /proc/nvme /proc/usbstorage   storage controller state
cat /proc/ntp          network time: did the clock get set, from whom      cat /proc/thermal, sensors   CPU temperature, frequency, power (SENSORS.md)
```

`hwreport -o` collects all of this (the SMBIOS strings, the CPU, every PCI function with its driver, disks, USB, network, audio, the warnings in the log) into one text file to send
([HARDWARE_DB.md](HARDWARE_DB.md)); `firstlook` appends it to its own file.
Photograph the screen if the machine dies before a shell exists; the last lines of the log say where. The log has one-line answers to
"what machine is this": the CPU features, the memory map summary, the framebuffer mapping and its measured write speed, the ACPI tables
read, how the TSC was calibrated, what the PS/2 controller and the USB ports hold. [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md) lists them,
what each should look like, and everything that was reviewed for this machine (and what could not be checked without it).

### The log on the stick (for a boot that fails, and for the next one)

The PC has no serial port, so once the HUMANVILLAGE volume is mounted read-write the kernel keeps its log there too, in two ordinary
files in the root of the stick's big partition (`/HumanVillage/TOUHOU.LOG`):

| File | Holds |
|------|-------|
| `TOUHOU.LOG` | this boot's `dmesg`, from the very first line; written when the volume is mounted, appended every ~5 seconds *if there are new lines*, and once more at poweroff/reboot, where it ends with `-- bootlog: the machine is being shut down ... this line ends a clean run --` |
| `TOUHOU.PRV` | the boot before (rotated at mount time), so a hang followed by a power cycle still leaves the failed boot readable |

Plug the stick into another computer (Windows, macOS and Linux all open the big partition; the 64 MiB one is the boot partition, leave it
alone) and open the files in any text editor. A file that **does not end with the "clean run" line** belongs to a boot that hung or was
cut off; its last lines are the last thing the kernel said (at most ~5 s before the end). The file never grows beyond 256 KiB: past that it
starts over with the newest part. Every write is followed by a sync, so the data is on the stick within a moment and the volume is almost
never left flagged dirty by the log itself.

What it does *not* do: a kernel panic is not written (every disk driver needs the scheduler and interrupts, which a panic has just given
up; the panic text is on the screen: photograph it), nothing is written when the volume is mounted read-only (a stick that was pulled
without shutting down is flagged dirty; at the next boot the kernel goes through it first and mounts it read-write when nothing is out of
place, so the log of the failed boot is rotated to `TOUHOU.PRV` as usual; only when the check finds real damage does it stay read-only, with the
file of the failed boot intact, see section 5), and `bootlog=off` on the kernel command line switches the whole thing off.

## 4. The command line (`cmdline.txt` on the ESP)

Edit it on any computer (the ESP is an ordinary FAT32 volume) or give `tools/mkimage.py --cmdline`. The default is
`video=native blk.ro`; the image prepared for this machine's owner also has `tz=EST5EDT,M3.2.0,M11.1.0` (US Eastern time). If the
clock is hours off on the first boot, the PC's clock chip holds local time (Windows does that): add `rtc=local`.

| Word | Meaning |
|------|---------|
| `video=native` | keep the screen mode the firmware chose (normally the monitor's native resolution) |
| `video=max` | the largest mode the display offers, up to 2560x1600 |
| `video=1280x800` | exactly that mode (default without any `video=`: 1024x768 with fallbacks) |
| `blk.ro` | register every SATA disk and NVMe drive read-only: **keep this for the first boots**. It does not apply to USB sticks: the stick you booted from is where `/HumanVillage` lives |
| `blk.rw=GUID` | the GPT partition with this unique partition GUID stays writable despite `blk.ro` (the rest of its disk does not): `install` writes it into the `cmdline.txt` of the disk it installs on, see [INSTALL.md](INSTALL.md) |
| `usbstorage=off` | do not drive USB sticks at all (they are listed by `lsusb` but never read or written) |
| `bootlog=off` | do not keep `TOUHOU.LOG` / `TOUHOU.PRV` on the stick |
| `nonet` | leave every network card alone (no I219/RTL8168/e1000 probing at all; the loopback still works): the escape hatch if the boot stops around the network lines |
| `noaudio` | leave the sound controller alone (HDA/AC'97 are not probed) |
| `netdiag` | the loader tries the firmware's own network driver (needs "Enable UEFI Network Stack" in the BIOS): DHCP request, register dump of the card, report into the kernel log (`[Hakurei] netdiag: ...`); see NETWORK.md |
| `ahci=off` | leave the SATA controllers alone (the AHCI driver does not probe at all): the escape hatch if the boot stops while the internal disk is looked at for the first time |
| `nvme=off` | the same for NVMe drives |
| `nosshd` | do not start the SSH server (it starts by default: [SSH.md](SSH.md)) |
| `update.trial=N`, `update.fellback` | put there by the loader, not by hand: the new kernel and initrd of a pushed update are on trial / the previous ones were started after a failed trial ([UPDATE.md](UPDATE.md)); `update.watchdog=SECONDS` changes the 300 seconds a trial start waits for `sudo update confirm` |
| `text`, `nogui` | no desktop, a text shell |
| `login` | ask who is at the door (login screen) instead of letting `reimu` in |
| `autologin=NAME` | open NAME's desktop instead of reimu's |
| `tz=EST5EDT,M3.2.0,M11.1.0` | the time zone (POSIX syntax, see `man timezone`); also what makes the file times on the stick come out in local time on other computers (FAT stores local time) |
| `rtc=local` | the clock chip keeps local time (a PC that also runs Windows does that) |
| `ntp=off` | no network time (by default the kernel sets the clock from an NTP server soon after the network comes up; [NTP.md](NTP.md)) |
| `ntp=HOST[:PORT][,...]` | NTP servers to ask when the DHCP lease names none (otherwise a built-in list of public ones is used) |
| `ntp.interval=SECONDS`, `ntp.rtc=1` | how often to ask again (default 14400); also write the clock chip after each sync (UTC, or local time with `rtc=local`) |
| `thermal=off` | read no CPU register at boot or later and leave the IDT alone: the escape hatch if the boot stops at the `[Utsuho]` lines ([SENSORS.md](SENSORS.md)) |
| `thermal.fake=MSR:VALUE,...`, `thermal.force`, `msrsafe.test`, `msrsafe.crash` | test hooks of the CPU sensor code ([SENSORS.md](SENSORS.md)) |
| `nohumanvillage` | do not mount the HUMANVILLAGE volume |
| `hvforce` | mount the HUMANVILLAGE volume read-write even if it was not closed properly **and** the check of its contents found damage. **Dangerous**: what is written to a damaged volume can make it worse; use it only to get at your files (copy them out first) |
| `hvcheck=off` | do not go through a dirty HUMANVILLAGE volume before mounting it: it is then read-only, as in the first versions (with `hvforce` it is forced) |
| `init=kshell` | skip userland, start the kernel's own tiny shell |
| `fb.cache=uc` | map the framebuffer uncached instead of write-combining: only for comparing the `framebuffer speed` log line when the screen feels slow (it also switches the MTRR fallback below off) |
| `fb.mtrr=auto` (default), `on`, `off` | the slow-screen fallback: when the screen measures under 400 MB/s the kernel tries a write-combining MTRR over the display adapter's BAR, logs `fb speed before N MB/s after M MB/s` and takes it back if the screen did not get faster. `on` tries it even when the screen is fast, `off` never touches the MTRRs |
| `ui.scale=N` | the desktop's ui scale (1 to 4): every logical pixel is N x N screen pixels. Without it the scale is 2 on screens at least 2560 wide or 1440 high (the Dell's monitor) and 1 otherwise; `ui.scale=1` keeps 1x there. See [DESKTOP.md](DESKTOP.md), "Hi-dpi screens" |
| `fairyd.copy=rep`, `fairyd.damage=bbox` | the window server's old drawing paths (byte-string copy, one damage rectangle): for comparing `/tmp/fairyd.stat` |
| `usb=debug` | log every USB enumeration step; `usb=off` leaves USB alone |

The loader cleans the file up (a UTF-8 byte order mark, UTF-16, CRLF line ends, several lines and tabs all become single spaces), so
any editor will do. If there is no `cmdline.txt` at all the loader uses `video=native blk.ro`.

## 5. What touches your disks

- Boot services read the kernel from the stick; the OS then runs from RAM.
- The kernel *reads* partition tables and the boot sectors of FAT-typed partitions to find a volume labelled
  `HUMANVILLAGE`. It mounts exactly that one, read-write, on `/HumanVillage` (see [STORAGE.md](STORAGE.md)). It never
  mounts, formats or probes anything else with writes.
- Root can still write `/dev/<disk>` by hand (`dd`) or `mount` a disk yourself. `blk.ro` makes the SATA and NVMe drivers
  refuse writes entirely, so even that cannot hurt a Windows disk.
- The stick you booted from is a disk inside TouhouOS too (USB mass storage): its big partition, labelled HUMANVILLAGE, is
  `/HumanVillage` and everything you save in your home directory survives the reboot. The 64 MiB boot partition on the same stick is
  never mounted. The kernel only *reads* the stick's partition tables and boot sectors to find the label; it writes only through the
  mounted HUMANVILLAGE volume (your files, `TOUHOU.LOG`) or when root writes `/dev/sda` by hand. A second stick that also carries a
  HUMANVILLAGE volume is ignored (the first one found wins; an internal disk that has one beats any stick), and any other stick is
  readable (`mount -t fat32 /dev/sdb1 /mnt`) but is never mounted by itself.
- Eject before you pull: `poweroff` (or the panel's shutdown) flushes and marks the volume clean. A stick that is pulled, or a machine that
  is switched off with the power button held, while the volume is flagged dirty (a file was written in the last few seconds) is **gone
  through before it is mounted** at the next boot: `[Keine] sda2 was not closed properly: Keine goes through the whole ledger, read-only,
  before anyone writes in it`. The check reads the whole FAT and every directory (about a second for a small stick; it never writes). Three
  outcomes:
  - *nothing is out of place* (the usual case: the cut came between two writes): `... nothing is out of place: the Human Village may write in
    it again`; the volume is mounted read-write, the flag is cleared again after the first sync, and `TOUHOU.LOG` is kept as usual;
  - *only harmless leftovers* (clusters that were allocated and never used, a file whose chain is a little longer than its size, a stale free
    count): mounted read-write too, and the log says so; the flag stays until a file system check tidies them up (`fsck -a`, below);
  - *damage* (a chain that runs into nothing, two files sharing clusters, a directory without `.` / `..`, ...): the volume stays
    **read-only**, nothing is written, `TOUHOU.LOG` is not updated, and the log (`dmesg | grep Keine`, and `cat /proc/fatcheck` with every
    problem) says precisely what and what to do: as root `umount /HumanVillage`, `fsck -a /dev/sda2` (`fsck -y` also repairs what needs a
    decision, such as a file whose chain is cut at its break), then reboot. Your files are still readable meanwhile. The walk-through, with
    what `fsck` prints, is below. (`fsck` has been run in QEMU guests on FAT12, FAT16 and FAT32 volumes, on 512- and 4096-byte sectors, and on
    the images that power cuts left behind; it has not yet been run on the real machine. On Windows, "Scan and fix" on the big partition, or
    `fsck.vfat -a` on Linux, do the same job.)
  `hvforce` on the kernel command line mounts the volume read-write even when the check found damage (dangerous), `hvcheck=off` skips the check
  (then a dirty volume is read-only, as before). From inside TouhouOS root can also mount it by hand
  (`umount /HumanVillage; mount -t fat32 -o force,userdirs /dev/sda2 /HumanVillage`), but the flag then stays dirty. If you pull the
  stick of a *running* system, `/HumanVillage` starts failing with "No such device" until you `umount /HumanVillage` and plug it in again.

### After a bad shutdown: what the log says and what to do

A power button held down, a stick pulled out, a hang that ended with the switch: the volume is flagged dirty. The next start goes through its ledger
before it decides how to mount it (it only reads; a second or so for a small stick, the check gives up after 120 s), and says what it found in one
`[Keine]` line: on the screen during boot, afterwards `dmesg | grep Keine`; `cat /proc/fatcheck` lists every finding.

| The line says | It means | What to do |
|---------------|----------|------------|
| `... nothing is out of place: the Human Village may write in it again` | only the dirty mark was left (the power went between two writes). The volume is mounted read-write (mount option `fsck=ok`) and the mark goes at the first idle flush or `sync` | nothing |
| `... found only harmless leftovers in the ledger (N lost cluster(s), M other ...): it is mounted read-write, and fsck -a /dev/sda2 as root tidies it up` | `fsck=left`: writable, but a few clusters were allocated and never used, a file's chain is a little longer than its size, or a FAT copy and the free count lag behind. Nothing points at wrong data; the leftovers only cost a little space. The dirty mark stays on the volume until `fsck -a` has been through it (so every start checks again, which is quick); a file with a surplus chain is trimmed when it is next written | not urgent: tidy it up as below when you have a quiet moment |
| `... its ledger does not add up: N error(s). It stays read-only so that nothing gets worse. As root, umount /HumanVillage and run fsck -a /dev/sda2` (or `... the check of its ledger could not be finished`: out of time or memory, or the stick failed) | real damage: a chain that runs into nothing, two files that share clusters, a directory without `.` and `..`, a name that is not one ... Nothing is written (so no `TOUHOU.LOG` for this boot either); every file that can be read still can | copy out what you need if it matters, then mend it as below |

The repair, from a text console (`Ctrl+Alt+Backspace` leaves the desktop). Root's login is locked: log in as `marisa` and use `sudo`.

```
cd /
sudo umount /HumanVillage
sudo fsck -n /dev/sda2              # look first; the name is in the log line (lsblk shows the disks)
sudo fsck -a /dev/sda2              # mend what loses nothing
sudo fsck -y /dev/sda2              # only if -a says "were not allowed: -y allows them"
sudo reboot
```

What it prints (abridged; a stick that lost a chain and had a bad name, `fsck -a` first):

```
fsck: Keine goes through the ledger of /dev/sda2 (FAT32, 80628 clusters of 512 bytes)
warning  volume is marked dirty (FAT[1] clean-shutdown bit is clear)
error    /reimu/he*lo.txt: illegal character '*' in the short name
error    /marisa/spell.dat: chain runs into a free cluster (cluster 37, after 4 cluster(s))
leftover 139 lost cluster(s) (allocated in the FAT but part of no file), first: 38, 39, 40, 41, 42, 43, 44, 45
/dev/sda2: 48 files in 10 directories, 80410 of 80628 clusters free
/dev/sda2: 2 error(s), 3 leftover(s) of a power cut, 2 warning(s)
  left alone (needs -y): /marisa/spell.dat: chain runs into a free cluster (cluster 37, after 4 cluster(s))
  fixed: saved a lost chain of 136 cluster(s) as FOUND.000/FILE0000.CHK
/dev/sda2: 4 repair(s) made in 4 pass(es), 2 lost chain(s) of 139 cluster(s) saved in FOUND.000
/dev/sda2: 2 error(s) and 0 leftover(s) remain (2 repair(s) were not allowed: -y allows them)
```

The exit status tells the same in a script: 0 nothing was wrong, 1 everything was mended, 4 problems are left (`-y`, or another computer's `chkdsk`),
8 the check could not be done (the volume is mounted, it is not FAT, only root may open the disk), 16 usage, 32 you stopped the questions with `q`.
`fsck` without `-n`, `-a` or `-y` asks before every repair (`y`, `n`, `a` = yes to all, `q`). After the repair the volume is clean: the next
start does not mention a ledger, and the files are readable and writable again.

* **What is kept, and what is not**: a lost chain is *saved*, not thrown away: it becomes a file `FOUND.000/FILE0000.CHK`, `FILE0001.CHK`, ... in the top
  folder of the volume (a whole number of clusters, so a little padding at the end). Look at them and keep what is yours. `-y` cuts a chain where it
  breaks (the file keeps what lies before the break, the rest becomes a `.CHK` file), empties or removes entries that point nowhere, and renames a
  name that cannot exist: the data stays in the file or in a `.CHK` file, but a name may go or a file end up shorter than it was.
* **`umount` says busy**: something still has a file open on the volume, usually the desktop (it keeps the home folders there). Leave the desktop
  first and `cd /` in the shell, or boot once with `nohumanvillage` added to `cmdline.txt` (nothing is mounted then, the homes are in memory),
  run `fsck` on the partition, take the word out again. This has been tried in QEMU with a root shell and with `nohumanvillage`; whether the
  desktop on the Dell lets go of the volume has not been tried.
* **`fsck` cannot help** when the boot sector is damaged (it says so and writes nothing), when the device itself fails, or when the volume is not
  FAT. On Windows, "Scan and fix" on the big partition, or `fsck.vfat -a` on Linux, can do the same job (Windows saves lost chains the same way,
  in `FOUND.000`; Linux's `fsck.vfat` calls them `FSCK0000.REC`).
* **Not tried on the Dell**: `fsck` and this walk-through. QEMU has shown: the options and exit codes on FAT12, FAT16 and FAT32 volumes, 512- and
  4096-byte sectors, the questions, the refusals, the flow above with a restart afterwards, and the images that random power cuts left behind
  (`tests/case_fsck.py`: `fsck_cmd_*`). What is worth doing once with the owner's go: switch the machine off with the power button while something
  writes (a big copy), start it again, read the `[Keine]` line and `cat /proc/fatcheck`, and run the three commands.

## 6. Known gaps and things to try if it does not work

- **No picture after the loader**: add `video=1024x768` (a mode every GOP offers), or `text`.
- **The text console or the desktop is slow**: the `framebuffer speed` log line says how fast the screen memory takes writes (hundreds of
  MB/s or more is healthy). Under 400 MB/s the kernel corrects itself: after the PCI scan it prints the firmware's MTRR layout and tries a
  write-combining MTRR over the display adapter's BAR (`framebuffer: write-combining MTRR #N ... fb speed before N MB/s after M MB/s, kept`, or
  the reason none was added); photograph those lines. Boot once with `fb.cache=uc` and compare if in doubt; on the desktop
  `cat /tmp/fairyd.stat` shows what a frame costs (compose = painting in RAM, flush = writing the screen).
- **Frozen right after the picture appears, or a reset a few seconds later**: see the checklist at the end of [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md).
- **Keyboard or mouse dead**: try another USB port without a hub; `cat /proc/usb` (from a USB device you do have, or
  read the log lines `[Kappa] xhci ...` on screen) shows whether the controller started and what it saw.
- **Wrong time**: once the network is up the kernel sets the clock itself (`cat /proc/ntp`, [NTP.md](NTP.md)); before that, and without a network, the clock is the chip's: `rtc=local` and `tz=`; fix the chip with `sudo hwclock -w` (see [POWER_AND_TIME.md](POWER_AND_TIME.md)).
- **Boots, then reboots**: the log's last lines name the subsystem; every driver polls and has timeouts, so a stall is
  normally one device being skipped after a few seconds, not a hang.
- **Sound** (Intel HD Audio, AC'97 as a fallback; never run on a real codec yet): `volume`, then `beep`; if the jack stays
  silent, `cat /proc/audio` and `dmesg | grep Nitori` say what the driver found; see [AUDIO.md](AUDIO.md).
- **Network** (see [NETWORK.md](NETWORK.md)): the I219-LM of the target machine (and RTL8168/8111 boards) have drivers that have
  never run on hardware. On boot the log shows `[Yukari] eth0: Intel I219-LM at ..., MAC ...` and, when a cable is plugged in
  and autonegotiation finishes, `link up, N Mb/s` followed by the DHCP lease line; `ifconfig`, `ip addr` and `cat /proc/net/dev`
  show the state, `ping GATEWAY` is the first test. A line saying `did not answer sensibly; leaving it alone` means the card
  was not touched. The driver never resets the card or the PHY (the firmware's setup is kept), so if there is no link at all,
  the usual suspects are a cable in a port the ME owns or a PHY in ultra-low-power mode that the ME did not release.
- **The stick is not found** (no `usb-storage` line in the log): `cat /proc/usb` shows whether the stick enumerated and with which
  driver (`driver usb-storage`); `class 08/06/62` means a UAS-only device, which is ignored; `cat /proc/usbstorage` shows the state of
  each unit (`waiting for the unit to become ready`, `given up (...)` with the reason, `no medium`). A slow stick may need more than
  the 8 s that boot waits for; it is then mounted a little after the desktop is up.
- Not implemented: suspend, multi-core, GPU acceleration (software drawing at the firmware's resolution is plenty for a desktop of this
  size); USB mass storage speaks only the bulk-only protocol (no UAS), so a very fast USB 3 SSD stick runs slower than it could.
