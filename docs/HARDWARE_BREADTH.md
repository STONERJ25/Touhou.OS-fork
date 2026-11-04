# Running on many machines: a plan for hardware breadth and a bootable ISO

**Goal:** people download an ISO, boot it on their own PC (any x86-64 UEFI machine), and play with the system without installing anything. This page lists what that takes, in the order it pays off, and how to measure instead of guess. Written 2026-11-03; the OS has run on one real machine (a Dell OptiPlex, Core i5-7500) and in QEMU.

## What is universal already, and what is not

| Layer | Today | Universal? |
|---|---|---|
| Boot | A UEFI loader (PE/COFF) from a FAT boot partition; Secure Boot must be off | every UEFI PC; not legacy BIOS (not planned) |
| Display | The UEFI GOP framebuffer, software drawing | every UEFI PC, **fixed at the boot-time mode** (no native mode setting) |
| Keyboard, mouse | USB HID over xHCI, PS/2 | desktops yes; many laptops have internal keyboards and touchpads on **I2C-HID**, not PS/2 |
| Storage | AHCI (SATA), NVMe, USB sticks; everything read-only except the `HUMANVILLAGE` volume | AHCI/NVMe yes; **Intel RST/VMD "RAID" mode hides drives** (seen on the Dell); older USB (EHCI/OHCI/UHCI) not done |
| Network | virtio, Intel e1000/e1000e/PCH, Realtek 8139/8168 | wired Intel and Realtek common cases; no Wi-Fi, no 2.5G Realtek/Intel yet |
| Audio | Intel HD Audio, AC'97 | HDA covers most boards; codec quirks vary |
| Interrupts, power | PIC/APIC with polling for most devices; ACPI tables parsed, soft-off, a quirk-prone reset | **no AML interpreter**, no MSI/MSI-X, one CPU used |
| GPU | none (GOP only) | |

The boot image holds the initrd in RAM, so a **live ISO needs no storage driver at all**: the firmware loads the loader, the loader loads the kernel and the initrd, and only the screen, a keyboard and (optionally) a `HUMANVILLAGE` stick are needed. That is the "try it, touch nothing" story, and it is already how the USB image works.

## Measure first: a hardware report

Which drivers matter is decided by what people actually own. Plan:

1. **`hwreport`**: a command on the live system that writes one text file (to the `HUMANVILLAGE` volume or `/tmp`): PCI ids with class and driver status, USB devices, CPUID and microarchitecture, SMBIOS/DMI strings (vendor, product, BIOS), the ACPI table list, GOP modes and the chosen one, the storage controllers and what was found behind them, network devices and link state, audio codecs, the boot log's warnings. (`firstlook` is the seed: it already writes the first-look file.)
2. **A compatibility list** in the docs (`docs/HARDWARE_DB.md`): one line per tested machine, from the reports, saying what worked.
3. **Ask testers for a report even when it fails to boot**: the kernel already keeps its log (`TOUHOU.LOG`) on the stick it booted from, so a failed start still leaves something to send.

## The driver backlog, by payoff

1. **Interrupts that work everywhere:** MSI/MSI-X for NVMe, xHCI and NICs (the polled paths work but burn CPU and add latency), IOAPIC routing from the MADT, **SMP** (the other cores idle), HPET/TSC timer checks. These help every machine.
2. **USB breadth:** EHCI plus the OHCI/UHCI companions (older boards route ports to EHCI until xHCI is switched on), hubs and hot-plug hardening, USB audio class, **USB networking for phones and dongles** (CDC-ECM/NCM, RNDIS, the common Realtek and ASIX adapters): this is the pragmatic network path for Wi-Fi-only laptops (tethering) until Wi-Fi exists.
3. **Storage:** Intel RST/VMD remapping (NVMe behind VMD), AHCI quirks (port multipliers are rare; DMA and link-power management are not), eMMC and SD host controllers (SDHCI) for small laptops.
4. **Wired network:** Realtek 8125 (2.5G) and the 8111/8168 variants, Intel I225/I226 (`igc`), Atheros/Qualcomm (`alx`), Broadcom (`tg3`). Wi-Fi is last: every chip needs a vendor firmware blob and a large driver; do not start it before the rest.
5. **Laptops:** an **ACPI AML interpreter** (battery, lid and power button, thermal zones, the embedded controller, device enumeration for I2C/GPIO), I2C-HID keyboards and touchpads (Intel LPSS I2C and GPIO interrupts), backlight, suspend (S3).
6. **Graphics:** native mode setting for Intel (the display engine's documentation is public; the Dell's HD 630 is the first target) gives resolution changes, multiple monitors and hot-plug. AMD's display engine is much bigger; NVIDIA has no public documentation: those stay on GOP. Acceleration (3D) is a separate, very large project; software rendering is the plan for games.
7. **Quirks as data:** a table keyed by DMI strings (vendor/product/BIOS) for the reset method, broken ACPI, devices to ignore, like the quirk list the Dell already forced.

## The ISO

* **Format:** an ISO 9660 image with an El Torito UEFI boot entry (an embedded FAT image holding `EFI/BOOT/BOOTX64.EFI`, the kernel and the initrd), also valid when written byte for byte to a USB stick (a hybrid GPT). A small original writer (`tools/mkiso.py`; the format is simple) is enough; no outside tool is required.
* **Behaviour:** boots to the desktop from RAM; `blk.ro` stays the default so that no disk is touched; the `HUMANVILLAGE` stick or partition is the only writable place; `install --alongside` is the explicit, separate step that already exists and has installed next to Windows without touching it.
* **Secure Boot:** the loader is unsigned, so users turn Secure Boot off (documented). Signing needs a Microsoft-signed shim: a later, separate decision.
* **Where people will try it first:** VirtualBox, VMware, Hyper-V (Generation 2), QEMU. Check each: VirtualBox and VMware present AHCI, e1000 and GOP (should work); **Hyper-V Generation 2 has no PS/2 and a VMBus-only keyboard and mouse**, so it needs either a VMBus input driver or a USB-attached input; say so in the documentation rather than discover it from bug reports.

## Testing without a hundred machines

* QEMU already covers the common controller models (Q35 with AHCI, NVMe, xHCI, e1000e, RTL8139, HDA, AC'97); add the missing models as drivers land (EHCI, SDHCI, more NICs, USB-network gadgets, a second GOP resolution, odd PCI layouts).
* The hardware audit's habit continues: reproduce what a real PC does that QEMU's defaults hide (D3hot function wake-up, 64-bit BARs, firmware-programmed MTRRs, a firmware that clears RAM at reset) as a test each time a real machine teaches it.
* A short list of friendly testers with different machines (an AMD desktop, an Intel laptop with I2C-HID, a Ryzen laptop, an older Core 2 board) is worth more than any amount of reading; their `hwreport` files decide the backlog above.

## Suggested order

1. `hwreport` and the hardware list (small; starts the data flowing), 2. MSI/MSI-X and SMP (everything gets better), 3. the ISO writer and a QEMU/VirtualBox boot test of it, 4. EHCI/OHCI/UHCI and USB networking, 5. the Realtek and Intel 2.5G cards, 6. Intel mode setting, 7. the AML interpreter and I2C-HID for laptops. Wi-Fi last.

## State (branch `agent/hwbreadth`, 2026-11-03)

Items 1 and 2 of "Measure first" and the "Format" and "Behaviour" bullets of "The ISO" are built; everything else on this page is still a plan (item 3 of "Measure first", asking testers for a report even when the machine fails to boot, is text in [HARDWARE_DB.md](HARDWARE_DB.md) only). A tick is for what exists **and has a test**.

- [x] **`hwreport`** (`user/bin/kappabox/hwreport.c`): sections system, cpu, memory, firmware, acpi, pci, storage, usb, network, audio, input, interrupts, sensors, log, cmdline, quirks; privacy by default (`--full` keeps serial
  numbers, UUIDs, MAC addresses); `-o` or a file name writes to the stick's volume, `/tmp` or the named file; `firstlook` appends it. Tests: `tests/case_hwreport.py` (QEMU: `python tools/qemu-slot.py -- python tests/run_all.py hwreport`).
  Cost in the boot set: kernel +34 KB, loader +2.5 KB, initrd +70 KB (the `kappabox` binary 28 KB, the copy of its source that the initrd ships for self-hosting 46 KB, the manual page 4 KB).
- [x] **Kernel interfaces**: `BootInfo` version 6 (`shared/bootinfo.h`, `boot/fwinfo.c`: the SMBIOS 2 or 3 table copied into loader memory, firmware vendor and revision, Secure Boot, the graphics modes the firmware offered);
  `/proc/dmi` (`kernel/arch/x86_64/dmi.c` and `dmi_parse.c`: SMBIOS types 0, 1, 2, 3, 4, 17 from DSP0134; host tests `tests/host/test_dmi.py`, with an AddressSanitizer run over every truncation and thousands of damaged tables);
  `/proc/hwinfo` (the memory map by type, the TSC rate, the ACPI table list with OEM ids, the MADT summary, which well-known devices the AML bytes mention, the interrupt set-up) and `/proc/pcicaps` (MSI, MSI-X, PCIe link speed and
  width per function), both in `kernel/arch/x86_64/hwinfo.c`.
- [x] **`tools/hwreport.py`**: reads one or many reports, prints a row for the table of [HARDWARE_DB.md](HARDWARE_DB.md) and (several reports, or `--drivers`) the PCI ids without a driver, most common first; `--check` finds an unredacted
  MAC address, UUID or serial number. Tests: `tests/host/test_hwreport.py` against reports from five QEMU machine types (`tests/host/hwreports/`).
- [x] **The compatibility list** [HARDWARE_DB.md](HARDWARE_DB.md): how to send a report, what is redacted, the line format, the first rows (the Dell from the audit documents; QEMU's Q35 and i440fx).
- [x] **The ISO** (`tools/mkiso.py`, [ISO.md](ISO.md)): a hybrid file of about 18.3 MiB that boots under UEFI as a CD (El Torito entry, platform `0xEF`, naming an embedded FAT16 image) and, written byte for byte to a USB stick, as a disk (protective MBR and a GPT
  whose ESP entry is the same image); the ISO's tree shares its bytes with the FAT image, so nothing is stored twice; reproducible with `SOURCE_DATE_EPOCH`. Checked by an independent reader (`tests/isocheck.py`), by `file`, `sgdisk`, `sfdisk` and 7-Zip,
  and by damaging the image sixteen ways (`tests/host/test_mkiso.py`: `python tools/hosttest.py discover -s tests/host -p "test_mkiso.py"`); booted in QEMU/OVMF as a CD and as a USB disk to the desktop, and in text mode with a blank disk attached
  (`tests/case_iso.py`: `python tools/qemu-slot.py -- python tests/run_all.py iso_`). **Unverified**: every real firmware, Rufus/`dd`/Etcher on a real stick, VirtualBox, VMware, Hyper-V (Generation 2 cannot work: no input device it has a driver for), `xorriso` and `isoinfo`.
- [ ] Everything under "The driver backlog" (MSI/MSI-X, SMP, EHCI/OHCI/UHCI, USB networking, other NICs, mode setting, the AML interpreter): not started, as the brief said.

What the first reports already show (QEMU only, so read it as what the machinery finds): no machine has a driver for its display adapter (the OS draws on the GOP framebuffer), and an i440fx machine's boot disk is a PIIX IDE
controller that nothing drives (the system finds no disk there; on a live ISO that does not matter). Nothing is measured yet on a real machine other than the Dell.

**Not verified on real hardware**: all of `hwreport` except what the Dell's earlier logs showed through the same `/proc` files. The SMBIOS walker has only seen QEMU's tables and the synthetic ones of the host tests; real firmware has odd
tables (padded strings, a UUID of all `0xFF`, structures shorter or longer than their version says), which is why the walker checks every length. The first real report is the real test.
