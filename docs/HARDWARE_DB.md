# Machines TouhouOS has run on (the compatibility list)

One line per machine, from the reports that testers send. The point is not to boast: **the lines that say what did not work decide which driver gets written next**
([HARDWARE_BREADTH.md](HARDWARE_BREADTH.md) has the plan). The system has run on one real PC so far; every other line here is waiting for you.

## How to add your machine

1. Boot TouhouOS (from the [ISO](ISO.md) or the [stick image](REAL_HARDWARE.md)) and open a terminal (Ctrl+Alt+T), or leave the desktop with Ctrl+Alt+Backspace.
2. Run `hwreport -o`. It writes `/HumanVillage/hwreport.txt` when the stick's volume is mounted (your home directory on the stick, if you may not write the top of it) and
   `/tmp/hwreport.txt` otherwise (an ISO has no such volume: `hwreport` alone prints the report on the screen, and `hwreport -o` keeps it in memory until you power off). To get the text
   out before you shut down: photograph the screen (`hwreport | less` pages it), or, if the machine has a network connection and another computer runs an SSH server, `scp /tmp/hwreport.txt you@that-computer:`
   (`man scp`; not tried for this purpose), or plug in a stick that has a `HUMANVILLAGE` volume before you run `hwreport -o` (the kernel mounts one that appears later; not tried from an ISO).
3. Send the file, and say in a sentence or two what worked and what did not: did the picture appear, keyboard, mouse, sound, disks, network, a clean shutdown (`poweroff`). A
   machine that **does not boot** is just as valuable: tell us the last line on the screen, and if a stick with a `HUMANVILLAGE` volume was in the machine, send `TOUHOU.LOG` from it
   ([REAL_HARDWARE.md](REAL_HARDWARE.md) says where it is).

An issue on the public repository (`github.com/STONERJ25/Touhou.OS-fork`) with the file attached is the place until there is another. Nobody will ask for money, an account or
anything else about you.

### What the report keeps to itself

A report is meant to be pasted into public places, so by default it **replaces** the serial numbers (system, board, chassis, memory modules, disks), every UUID and a host name
other than the default, and **cuts the last three bytes** off every MAC address (`52:54:00:xx:xx:xx` keeps the maker's prefix and nothing that identifies your card); a private IP address of a network card loses its last
byte and a public one is not shown. The fourth line of the report says whether this was done. `hwreport --full` keeps everything, for the owner of the machine: do not post that file.
What stays in is what makes the report useful: the vendor, product and BIOS strings, the CPU model, the PCI and USB ids, the disk models and sizes, the ACPI table list, and the warnings in the
kernel log. Look through the file before you send it; `python tools/hwreport.py --check FILE` checks mechanically that nothing in it looks like a full MAC address, a UUID or a serial number.

### What the report is

Plain text. A first line `format: hwreport 1`, a few header lines (`build:`, `date:`, `redaction:`), then sections in a fixed order, each `[name]` followed by `key: value` lines (a key may
repeat), and a last line `end: hwreport 1` (a file without it was cut short). Missing information is a line that says so, never an empty section.

| Section | What is in it |
|---|---|
| `[system]` | the firmware's own words (SMBIOS): BIOS vendor, version and date; system, board and chassis strings |
| `[cpu]` | vendor, brand, family/model/stepping, a guess at the microarchitecture, feature flags, logical processors, TSC rate, hypervisor |
| `[memory]` | the firmware's memory map summed by type, and the memory modules the SMBIOS lists |
| `[firmware]` | UEFI vendor and version, Secure Boot, the graphics mode in use and every mode the firmware offered |
| `[acpi]` | the table list with OEM ids and revisions, the MADT (processors, I/O APICs, interrupt overrides), which devices the AML mentions |
| `[pci]` | **every PCI function** with ids, class, the driver that took it (`none` if none did), MSI/MSI-X, the PCIe link, BAR sizes |
| `[storage]` | controllers (AHCI, NVMe, RAID/VMD, IDE ...) and the disks behind them, with size and model |
| `[usb]` | host controllers by type (xHCI, EHCI, OHCI, UHCI) with their drivers, and the devices on them |
| `[network]` | each card with its driver, link state and speed; PCI network controllers without a driver |
| `[audio]` | controllers, the codec ids, whether a pin configuration was usable |
| `[input]` | keyboards and mice, by USB or PS/2 |
| `[interrupts]` | how the kernel handles interrupts here (it polls; MSI and SMP are not used yet) |
| `[sensors]` | temperature sensors found |
| `[log]` | the kernel log lines that mention a warning, error, failure, unsupported feature, timeout or something that was "no" (up to 100) |
| `[cmdline]`, `[quirks]` | the kernel command line; the workarounds applied (this kernel keeps no such table yet) |

## Reading many reports: `tools/hwreport.py`

```
python tools/hwreport.py report.txt               one line for the table below
python tools/hwreport.py reports/ --header        every *.txt in the folder as table rows, then the driver backlog
python tools/hwreport.py --drivers --all report.txt
python tools/hwreport.py --check reports/         is each file whole, and is nothing private in it?
```

The second part of the output is the one that matters for the project: **the PCI ids that no driver took, and in how many reports they appear**, most common first (bridges and other
devices that never need a driver are hidden unless `--all`). Every report in this repository's tests (`tests/host/hwreports/`) comes from a QEMU machine, so the first thing it shows
is that no machine has a driver for its display adapter: the OS draws on the firmware's framebuffer (GOP) and has no graphics driver at all.

## The line format

One row per machine: the first part from the report (`tools/hwreport.py` fills it in), the last part from what the tester saw.

| Column | Meaning |
|---|---|
| Machine | the SMBIOS manufacturer and product (or the board's) |
| CPU | the brand string, with the microarchitecture the table in `hwreport` guessed |
| Firmware | BIOS vendor, version and date, UEFI version |
| Boot | `ok`, or where it stopped (the last line on the screen) |
| Display | the resolution the loader got from the firmware (GOP) |
| Keyboard, Mouse | `usb`, `ps/2`, or `none` (many laptops use I2C-HID, which TouhouOS cannot drive yet) |
| Storage | which controllers were seen, how many disks; `RAID mode hides drives` for Intel RST/VMD |
| Network | driver and link state, or the PCI id that has no driver |
| Audio | driver and codec id, or the PCI id that has no driver; add `silent` if the driver ran and nothing could be heard |
| Shutdown | `ok` (powers off by itself), `reset` (restarts instead), `hangs`, or `?` |
| Notes | anything else a person noticed |
| Build | the commit, or `built DATE` when the build did not record one |
| Date | the day of the report |

`?` means nobody has checked. A value that was not tested on the machine itself is marked `(unverified)`.

## The list

| Machine | CPU | Firmware | Boot | Display | Keyboard | Mouse | Storage | Network | Audio | Shutdown | Notes | Build | Date |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Dell OptiPlex desktop | Intel Core i5-7500 (Kaby Lake), 3.40 GHz, 4 cores (one used) | Dell UEFI, ACPI 5, Secure Boot off | ok, from a USB stick | 2560x1440 (GOP), write-combining through PAT, a full-screen fill takes 1.6 ms | usb (behind a hub) | usb (behind a hub) | USB stick (usb-storage, bulk-only): ok; the SATA controller is in **Intel RST (RAID) mode** and hides the internal disks (switch it to AHCI in the firmware setup) | Intel I219-LM: card found and its address read; **no link was reported** in the first log (no cable plugged in, or the PHY asleep): unverified | Intel HDA with a Realtek codec (`10ec:0255`): found, speaker/line/headphone outputs listed; playback not heard in that log: unverified | ok (the stick's log ends with the clean-run line) | the loader needed the "ordinary memory types" fix: OS-defined types made the firmware's `ExitBootServices` hang; the PS/2 controller answers neither keyboard nor mouse; the CMOS clock was wrong; boot to desktop took 1.5 s of kernel time | audit branch, 2026-10-23 (see [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md)) | 2026-10-23 |
| QEMU Standard PC (Q35 + ICH9) with Touhou Test SMBIOS strings (the reference machine of `tests/case_hwreport.py`) | QEMU TCG CPU (the host decides) | EDK II (OVMF), UEFI 2.7 | ok | 1024x768 (GOP) | usb+ps/2 | usb+ps/2 | AHCI, NVMe, 3 disks | e1000e (82574L) up, 1000 Mb/s | Intel HDA (`1af4:0012` codec) | ok | every driver the OS has runs on a QEMU model; the display adapter (`1234:1111`) has no driver by design: the framebuffer is the GOP's | 2026-11-03 branch `agent/hwbreadth` | 2026-11-03 |
| QEMU Standard PC (i440FX + PIIX) | QEMU TCG CPU | EDK II (OVMF), UEFI 2.7 | ok | 1024x768 (GOP) | usb+ps/2 | usb+ps/2 | **no disk visible**: the PIIX IDE controller (`8086:7010`) has no driver, so the boot volume is not readable from inside (harmless for a system that runs from memory) | virtio-net up | Intel HDA | ok | the findings above come from the QEMU runs in `tests/host/hwreports/` | 2026-11-03 branch `agent/hwbreadth` | 2026-11-03 |

(The rows are written by hand from reports; run `tools/hwreport.py` on a new report to get the first columns, then add what you saw. Keep rows short: details belong in the issue.)

## Not in the list yet, and why it matters

A friendly tester with an **AMD desktop**, an **Intel laptop with an I2C keyboard and touchpad**, a **Ryzen laptop**, a **Core 2 era board with only EHCI/UHCI USB**, a PC with an **Intel VMD or
RST RAID** controller, or one of the **virtual machines people will try first** (VirtualBox, VMware, Hyper-V generation 2) is worth more than any amount of reading: the reports of these machines
order the driver backlog in [HARDWARE_BREADTH.md](HARDWARE_BREADTH.md).
