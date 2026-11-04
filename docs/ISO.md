# The live ISO

One file, `touhou-os.iso` (about 18 MiB), that starts a whole TouhouOS **from memory** on any x86-64 PC or virtual machine with UEFI firmware, without installing anything and without touching the disks of the machine.
Boot it, look around, press Ctrl+Alt+T for a terminal, run `hwreport -o` and tell us what your hardware is (see [HARDWARE_DB.md](HARDWARE_DB.md)).

This page says what the ISO is and is not, how to make it, how to write it to a stick or give it to a virtual machine, and **which of its statements nobody has checked yet**: those are marked *(unverified)*.
Everything that was run is listed under "What was tested" at the end.

## What it is, and what it is not

* **UEFI only.** There is no BIOS/legacy boot. A machine set to "Legacy" or "CSM" mode will not start it; set the firmware to UEFI.
* **Secure Boot must be off.** The loader is not signed (see below).
* **It runs from memory and keeps nothing.** There is no `HUMANVILLAGE` volume on an ISO (the volume that normally keeps the home directories, [STORAGE.md](STORAGE.md)): your files, your settings and the passwords you set are gone at power-off. `hwreport -o` writes `/tmp/hwreport.txt` there, so copy the text out (or just run `hwreport` and read or photograph the screen) before you shut down.
* **It does not touch the PC's disks.** The default kernel command line is `video=native blk.ro`: `blk.ro` makes every SATA and NVMe disk read-only for the whole session, and the system mounts only a volume labelled `HUMANVILLAGE`, of which the ISO has none. A stick you plug in later is read, not changed, unless you mount it and write on purpose.
* **It is not an installer.** Putting TouhouOS on a disk is a different, explicit step that you type yourself, with confirmations: `sudo install` (see [INSTALL.md](INSTALL.md), `man install`), for example `install --alongside` to add it next to Windows in free space. Nothing in the ISO starts it. Booted from a virtual CD the system does not even know where it came from, so the installers refuse (they say what they need and "nothing was written"); booted from a stick written byte for byte, `install --dry-run DISK` plans from the stick's own boot partition.
* **What does not exist yet** ([HARDWARE_BREADTH.md](HARDWARE_BREADTH.md)): Wi-Fi, a graphics driver other than the firmware's framebuffer (so the resolution is the one the firmware chose), suspend, more than one processor, keyboards and touchpads that are I2C devices (many laptops' own), USB networking, Realtek 2.5G and Intel I225 network cards.

## Making the ISO

```
python tools/mkiso.py                  builds everything first (tools/build.py), then writes build/touhou-os.iso
python tools/mkiso.py --no-build       uses build/esp as it is
python tools/mkiso.py --out E:/touhou.iso --cmdline "video=1280x800 blk.ro"
```

No outside program makes the image: `tools/mkiso.py` is original code written from ECMA-119 (ISO 9660), the El Torito specification and the UEFI specification, and reuses the FAT and GPT writers of `tools/mkdisk.py`.
It prints the size (the FAT image of the boot partition plus about 80 KiB: the 32 KiB system area, the descriptors, the directories, the three text files and the backup partition table) and the SHA-256.
With `SOURCE_DATE_EPOCH` set, every time stamp comes from it and the same inputs give the same bytes; without it the clock is used. The identifiers (the disk GUID and the partition GUID) are derived from the contents.
Other kernel command lines (`text`, `video=max`, `tz=...`) are chosen at build time with `--cmdline`; there is no way to edit it on the finished image except by writing the stick and editing `cmdline.txt` on its boot partition.

### What is in the file

| Where | What |
|---|---|
| bytes 0 to 32 KiB (the ISO's "system area") | a protective MBR and a GPT whose one entry, an EFI System Partition, covers the FAT image below |
| block 16, 17, 18 | the ISO 9660 primary volume descriptor (volume id `TOUHOUOS`), the El Torito boot record, the terminator |
| block 19 | the boot catalog: a validation entry for platform `0xEF` (UEFI) and one bootable, no-emulation entry that names the FAT image |
| then | the path tables (little-endian and big-endian), the directories, `README.TXT`, `LICENSE`, `THIRD_PARTY_NOTICES.TXT` |
| the FAT image (about 18.2 MiB) | a FAT16 volume labelled `TOUHOUOS` with 2 KiB clusters: `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `initrd.img`, `cmdline.txt`; this is the El Torito boot image **and** the GPT's partition |
| the last 17 KiB | the backup GPT (header in the very last 512-byte sector) |

The ISO's own tree shows `README.TXT`, `LICENSE`, `THIRD_PARTY_NOTICES.TXT`, `EFI/BOOT/BOOTX64.EFI`, `KERNEL.ELF`, `INITRD.IMG`, `CMDLINE.TXT` and `ESP.IMG`. **The last five do not take any space of their own**: their extents are the bytes inside the FAT image
(the image is laid out so that every file starts on a 2 KiB boundary), which is why the file is hardly larger than the boot partition. Any operating system that can open an ISO shows them; a firmware that reads the disc as an ISO 9660 volume instead of through El Torito would find the loader at `EFI/BOOT/BOOTX64.EFI` and the kernel next to it *(unverified: no firmware tried)*.

## Writing it to a USB stick (a real PC)

The file is a disk image as well as a disc image: write it **as an image, not as a file**. Everything on the stick is replaced; check the drive letter or device name twice.

* **Rufus** (Windows): choose the stick, press SELECT and pick the `.iso`; when Rufus asks how to write it, choose **DD Image mode** (not "ISO Image mode", which would make its own layout). If Rufus does not ask (it offers the choice for images it recognises as hybrid), use a tool that copies an image byte for byte: balenaEtcher, Win32 Disk Imager, or `dd`. *(unverified: Rufus not run, and whether it recognises this image)*
* **Linux, macOS**: `sudo dd if=touhou-os.iso of=/dev/sdX bs=4M status=progress conv=fsync` (on macOS use the raw device, `/dev/rdiskN`, after `diskutil unmountDisk`). *(unverified on a real stick; the same bytes boot as a USB disk in QEMU, see the tests)*
* **balenaEtcher**: "Flash from file", the `.iso`, the stick. *(unverified)*

Afterwards the stick looks odd to Windows and to disk tools: one small partition that Windows does not show a letter for, "the backup partition table is not at the end of the disk" (the stick is larger than the 18 MiB image) and an offer to format it. **Do not format it, and do not "repair" anything**: that is the normal look of a hybrid image (the image of `tools/mkimage.py` shows the same message). To use the stick for something else later, clear it with the maker's tool or `diskpart > clean`.

Then boot the stick: open the firmware's one-time boot menu (F12, F11, F10, F8 or Esc, depending on the maker), choose the entry that names the stick **and says UEFI**, and have Secure Boot off (below). If the PC offers the stick twice, once with UEFI and once without, choose the UEFI one.

### Secure Boot

The loader is not signed, so a PC with Secure Boot on refuses to start it (usually with a message about a security violation, or it silently moves on to the next boot entry). Turn Secure Boot off in the firmware setup (F2, Del or F10 at power-on; the setting is usually on a "Security" or "Boot" page and may need a supervisor password to be set first). Two cautions in general terms:
a Windows PC that encrypts its drive (BitLocker, "device encryption") may ask for the recovery key at its next start after the setting was changed, so suspend BitLocker first or have the key at hand; and turn Secure Boot back on when you are done if you want it. Getting a signed loader would take a Microsoft-signed shim: a later, separate decision ([HARDWARE_BREADTH.md](HARDWARE_BREADTH.md)). *(All of this is general knowledge about firmware, not checked on a particular machine.)*

## Trying it in a virtual machine

| Where | How | State |
|---|---|---|
| **QEMU** | with OVMF as the firmware: `qemu-system-x86_64 -machine q35 -m 1G -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd -drive if=pflash,format=raw,file=OVMF_VARS.fd -cdrom touhou-os.iso`; or as a USB disk: `... -drive if=none,id=u,format=raw,readonly=on,file=touhou-os.iso -device qemu-xhci -device usb-storage,drive=u,bootindex=0`. Add `-display gtk` (or `sdl`) and a keyboard and mouse come with it (PS/2). | **tested**: both ways reach the desktop (`tests/case_iso.py`) |
| **VirtualBox** | New machine, type Other/Unknown (64-bit); *Settings > System > Motherboard > Enable EFI (special OSes only)*; *Settings > Storage*: put the ISO in the optical drive; leave the storage controller at SATA. The network card (Intel PRO/1000, the default is fine) and AC'97 or HD audio are drivers the system has. | *(unverified: not run)* |
| **VMware** (Workstation, Player, Fusion) | New machine, guest OS Other / Other 64-bit; before the first start edit the `.vmx` or *VM > Settings > Options > Advanced > Firmware type*: **UEFI**; leave *Secure Boot* off; connect the ISO as the CD/DVD. | *(unverified: not run)* |
| **Hyper-V, Generation 2** | **Does not work yet, and there is no workaround here.** A Generation 2 machine has no PS/2 and no USB controller for the console; its keyboard and mouse are VMBus devices (the "Synthetic Keyboard" and "Synthetic Mouse"), which this system has no driver for, so the desktop would come up with nothing to type on. It would also need the machine's *Secure Boot* checkbox off. Generation 1 is a BIOS machine and cannot start a UEFI-only ISO. Making Generation 2 work needs a VMBus input driver ([HARDWARE_BREADTH.md](HARDWARE_BREADTH.md)); until there is one, use another virtual machine product. | not usable *(from what the plan knows of Hyper-V Gen 2; not run)* |
| **A real PC from a virtual optical drive** (a BMC/IPMI "virtual media", a USB DVD drive with a burnt disc) | the same as the CD in QEMU. | *(unverified)* |

The system's drivers in a virtual machine are the ones QEMU has models for ([HARDWARE_DB.md](HARDWARE_DB.md)): SATA/AHCI, NVMe, xHCI, Intel HD Audio and AC'97, e1000/e1000e, RTL8139, virtio. VirtualBox's and VMware's models of these (AHCI, e1000, HD Audio) are the same families *(unverified)*. If a virtual machine shows a black screen, give it more video memory or the VGA/"VMSVGA" adapter; the loader needs the firmware's graphics output (GOP) and nothing else.

## What happens at boot

The firmware reads the El Torito entry (or the GPT's ESP when it is a stick) and starts `EFI/BOOT/BOOTX64.EFI`, the loader. The loader reads `cmdline.txt`, `kernel.elf` and `initrd.img` from the same FAT volume, asks the firmware for a graphics mode and the SMBIOS table, leaves the firmware and starts the kernel, which unpacks the initrd into memory. From a CD the loader logs `the boot volume's device path names no partition`
(a disc has none); from a stick it logs `booted from partition 1 (GPT, start 120, ...)`. The kernel then finds no volume labelled `HUMANVILLAGE` (`no HUMANVILLAGE volume found: homes stay in RAM`), the home directories of `reimu`, `marisa` and `sakuya` are made in memory, and `init` opens the desktop for `reimu` (no password). `poweroff` (or the panel's power menu) shuts down; there is nothing to flush.

## What was tested

On a Windows PC with QEMU and OVMF, 2026-11-03, `python tools/qemu-slot.py -- python tests/run_all.py iso_` and `python -m unittest discover -s tests/host -p "test_mkiso.py"`:

* the structure, by a reader written separately from the writer (`tests/isocheck.py`: ECMA-119 descriptors, directory records, ordering, both path tables, the El Torito catalog and its checksum, the protective MBR, both GPT headers with their CRCs, the FAT16 volume inside, that every shared file is the same bytes in both views), by `file` ("ISO 9660 CD-ROM filesystem data ... (bootable)"), `sgdisk -v` ("No problems found"), `sfdisk -d` and 7-Zip (it lists the tree and the El Torito image) — and by damaging the image in sixteen ways to see that the reader notices each;
* reproducibility (the same `SOURCE_DATE_EPOCH` gives the same SHA-256);
* QEMU/OVMF boots it as a CD and as a raw USB disk to the desktop; in text mode from the CD, `install` and `install --alongside` refuse, a blank disk stays byte for byte as it was, nothing complains about the missing volume, also at shutdown; from the USB disk `install --dry-run` plans from the stick's boot partition.

**Not tested** *(everything marked unverified above)*: any real firmware, a real stick written by Rufus, `dd` or Etcher, a burnt disc, VirtualBox, VMware, Hyper-V, Secure Boot on, `xorriso` and `isoinfo` (not installed on the machine this was written on; the host test runs them when they are there), mounting the ISO on Linux or macOS.
The partition table is written for 512-byte sectors, which is what nearly every USB stick has; a stick or card reader that presents 4096-byte logical sectors would need a differently laid out table (not made).
The El Torito catalog holds one entry (the validation entry for platform `0xEF` and one default entry); some firmware looks only at section entries, in which case a second entry in a section would be the fix. Reports of machines that did not start it are the most useful kind: [HARDWARE_DB.md](HARDWARE_DB.md).
