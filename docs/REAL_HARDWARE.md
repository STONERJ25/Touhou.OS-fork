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
3. `init` opens the desktop for `reimu` (no password), the HakureiDE desktop with a panel. Press **Ctrl+Alt+T** for a
   terminal, **Super+L** to lock, the power button for the shutdown dialog.

If something is wrong, the useful things to look at (in a terminal window or the text console, `Ctrl+Alt+Backspace` leaves the desktop):

```
dmesg | less           the whole boot log        cat /proc/pci        every PCI device and who claimed it
cat /proc/acpi         what ACPI told us          cat /proc/usb        the USB device tree (keyboard, mouse, hubs)
cat /proc/partitions   disks the kernel sees      cat /proc/ahci /proc/nvme   storage controller state
```

Photograph the screen if the machine dies before a shell exists; the last lines of the log say where.

## 4. The command line (`cmdline.txt` on the ESP)

Edit it on any computer (the ESP is an ordinary FAT32 volume) or give `tools/mkimage.py --cmdline`. The default is
`video=native blk.ro`; the image prepared for this machine's owner also has `tz=EST5EDT,M3.2.0,M11.1.0` (US Eastern time). If the
clock is hours off on the first boot, the PC's clock chip holds local time (Windows does that): add `rtc=local`.

| Word | Meaning |
|------|---------|
| `video=native` | keep the screen mode the firmware chose (normally the monitor's native resolution) |
| `video=max` | the largest mode the display offers, up to 2560x1600 |
| `video=1280x800` | exactly that mode (default without any `video=`: 1024x768 with fallbacks) |
| `blk.ro` | register every SATA disk and NVMe drive read-only: **keep this for the first boots** |
| `text`, `nogui` | no desktop, a text shell |
| `login` | ask who is at the door (login screen) instead of letting `reimu` in |
| `autologin=NAME` | open NAME's desktop instead of reimu's |
| `tz=EST5EDT,M3.2.0,M11.1.0` | the time zone (POSIX syntax, see `man timezone`) |
| `rtc=local` | the clock chip keeps local time (a PC that also runs Windows does that) |
| `nohumanvillage` | do not mount the HUMANVILLAGE volume |
| `init=kshell` | skip userland, start the kernel's own tiny shell |

## 5. What touches your disks

- Boot services read the kernel from the stick; the OS then runs from RAM.
- The kernel *reads* partition tables and the boot sectors of FAT-typed partitions to find a volume labelled
  `HUMANVILLAGE`. It mounts exactly that one, read-write, on `/HumanVillage` (see [STORAGE.md](STORAGE.md)). It never
  mounts, formats or probes anything else with writes.
- Root can still write `/dev/<disk>` by hand (`dd`) or `mount` a disk yourself. `blk.ro` makes the SATA and NVMe drivers
  refuse writes entirely, so even that cannot hurt a Windows disk.
- Reading the stick *from inside* TouhouOS needs USB mass storage, which is not part of this tree yet: the OS runs from
  RAM, `/HumanVillage` is the in-memory tmpfs, and nothing you do in a session survives a reboot.

## 6. Known gaps and things to try if it does not work

- **No picture after the loader**: add `video=1024x768` (a mode every GOP offers), or `text`.
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
- Not implemented: sound, USB mass storage, suspend, multi-core, GPU acceleration (software drawing at the
  firmware's resolution is plenty for a desktop of this size).
