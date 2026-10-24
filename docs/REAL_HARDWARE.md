# Running TouhouOS on a real PC

**Everything here has only ever run in QEMU.** The drivers follow the specifications and Linux's behaviour, and they are
written to fail soft (log one line, stay out of the way), but the first boot on real hardware is an experiment. The
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
   stick**: that is what marks the volume clean; a stick that was just pulled is mounted read-only at the next boot (see below).

If something is wrong, the useful things to look at (in a terminal window or the text console, `Ctrl+Alt+Backspace` leaves the desktop):

```
dmesg | less           the whole boot log        cat /proc/pci        every PCI device and who claimed it
cat /proc/acpi         what ACPI told us          cat /proc/usb        the USB device tree (keyboard, mouse, hubs, sticks)
cat /proc/partitions   disks the kernel sees      cat /proc/ahci /proc/nvme /proc/usbstorage   storage controller state
```

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
without shutting down is flagged dirty and mounted read-only on the next boot; the file of the failed boot is then still intact, and only
a file system check clears the flag again, see section 5), and `bootlog=off` on the kernel command line switches the whole thing off.

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
| `usbstorage=off` | do not drive USB sticks at all (they are listed by `lsusb` but never read or written) |
| `bootlog=off` | do not keep `TOUHOU.LOG` / `TOUHOU.PRV` on the stick |
| `text`, `nogui` | no desktop, a text shell |
| `login` | ask who is at the door (login screen) instead of letting `reimu` in |
| `autologin=NAME` | open NAME's desktop instead of reimu's |
| `tz=EST5EDT,M3.2.0,M11.1.0` | the time zone (POSIX syntax, see `man timezone`); also what makes the file times on the stick come out in local time on other computers (FAT stores local time) |
| `rtc=local` | the clock chip keeps local time (a PC that also runs Windows does that) |
| `nohumanvillage` | do not mount the HUMANVILLAGE volume |
| `init=kshell` | skip userland, start the kernel's own tiny shell |
| `fb.cache=uc` | map the framebuffer uncached instead of write-combining: only for comparing the `framebuffer speed` log line when the screen feels slow (it also switches the MTRR fallback below off) |
| `fb.mtrr=auto` (default), `on`, `off` | the slow-screen fallback: when the screen measures under 400 MB/s the kernel tries a write-combining MTRR over the display adapter's BAR, logs `fb speed before N MB/s after M MB/s` and takes it back if the screen did not get faster. `on` tries it even when the screen is fast, `off` never touches the MTRRs |
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
  is switched off with the power button held, while the volume is flagged dirty (a file was written in the last few seconds) is mounted
  **read-only** at the next boot: the kernel says so in the log (`read-only: the volume was not cleanly unmounted`), nothing is written to
  it, and `TOUHOU.LOG` is not updated. The data is fine; only a file system check clears the flag: plug the stick into Windows ("Scan and
  fix"), or use `fsck.vfat -a` on Linux. From inside TouhouOS root can mount it read-write by hand
  (`umount /HumanVillage; mount -t fat32 -o force,userdirs /dev/sda2 /HumanVillage`), but the flag then stays dirty. If you pull the
  stick of a *running* system, `/HumanVillage` starts failing with "No such device" until you `umount /HumanVillage` and plug it in again.

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
- **Wrong time**: `rtc=local` and `tz=`; fix the chip with `sudo hwclock -w` (see [POWER_AND_TIME.md](POWER_AND_TIME.md)).
- **Boots, then reboots**: the log's last lines name the subsystem; every driver polls and has timeouts, so a stall is
  normally one device being skipped after a few seconds, not a hang.
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
