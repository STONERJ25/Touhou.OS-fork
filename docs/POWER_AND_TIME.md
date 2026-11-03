# Power, ACPI and the clock

## ACPI

The loader hands the kernel the RSDP it found in the UEFI configuration table (`BootInfo.rsdp_phys`); `acpi_init()`
(`kernel/arch/x86_64/acpi.c`) walks RSDP -> XSDT (or RSDT) and reads:

* **FADT** -- the PM1a/PM1b control blocks (to power off), the PM1 event blocks (the power button), the SMI command
  port and ACPI-enable value (for firmware that has not switched to ACPI mode yet), and the reset register.
* **DSDT** (and SSDTs, if the DSDT has none) -- the `\_S5` object, found by scanning the AML for its byte pattern. That
  gives the SLP_TYP values that mean "soft off". There is no AML interpreter; every PC firmware encodes `_S5` the same way.
* **MADT** -- how many processors the machine has (only the boot processor is used, and `/proc/acpi` says so).

Tables that live outside the kernel's direct map are reached through `ioremap`. `cat /proc/acpi` shows what was found.

| action | what happens |
|--------|--------------|
| `poweroff`, *Shut down* | `acpi_poweroff()`: write SLP_TYP|SLP_EN to PM1a_CNT (and PM1b_CNT). QEMU's own shortcut ports are tried after it |
| `reboot`, *Restart* | the ways in turn until the machine goes: `acpi_reset()` (the FADT reset register, I/O port or PCI config space), PCI reset control 0xCF9, the keyboard controller pulse. Each way is shown on the screen and announced by N beeps of the PC speaker before way N; when none works the machine is switched off with a message instead of being left frozen. `reboot WAY` starts with one (`acpi pci kbd efi triple`), `reboot=efi,pci` on the command line sets the order; `cat /proc/acpi` says what a plain restart tries. Three ways are only tried when named: the ACPI register when it is the SMI command port, the firmware's `ResetSystem`, and a triple fault (see below) |
| the **power button** | a kernel thread polls the PM1 status register (`PWRBTN_STS`, 200 ms). With a desktop running the press arrives as `KEY_POWER`: the window server sends the shell a hotkey and the *Leave the shrine?* dialog opens; a second press powers off. On the login screen one press powers off. Without any window server the kernel powers the machine off itself |

### What the Dell OptiPlex 7050 showed about restarting (2026-10-25)

The first remote update ended with `restart` leaving the machine awake, its screen frozen on the old picture and its network dead. Three things came out of finding why:

- **The FADT's reset register is the SMI command port**: `io 0xb2 = 0x73` (`cat /proc/acpi` shows it). Writing it is a software system management interrupt, and with that register as the first way the machine froze at once, before the PCI way (which does restart it) was
  reached. Presumably the firmware's handler never comes back (SMM wants every processor to arrive, and the kernel puts the other processors into the INIT state at boot); that was not
  checked. `acpi_reset_is_smi()` keeps such a register out of the default order.
- **The PCI reset port 0xCF9 restarts it.** `sudo reboot pci` over ssh restarted the Dell by itself (screen down, back 29 s later; the restart was proved by a token in `/tmp` being gone, see [UPDATE.md](UPDATE.md)).
  It is first in the default order on that machine now, followed by the keyboard controller.
- **The firmware's `ResetSystem` faults.** The loader hands the kernel the service's address (`BootInfo.efi_reset_system`); `SetVirtualAddressMap` is never called, so the service runs in physical mode, and the kernel
  (`kernel/arch/x86_64/efireset.c`) builds a page table for just that call (upper half shared with the kernel's, lower half an identity map of 2 MiB pages, write-back where the memory map has memory, uncached elsewhere below
  4 GiB) and calls it through a Microsoft-x64 thunk (`efi_call.asm`). QEMU's OVMF restarts through it; on the Dell the CPU took a #GP at `rip=0x1000` (error code 0x3000, a segment-selector error) inside the firmware's own
  reset code, with the temporary page table active. Nobody knows yet why the firmware jumps to low memory there; the way stays as `reboot efi` / `reboot=efi` for machines where it works and is left out of the default order.

Tests: `power_restart_ways` (each way alone, in QEMU/OVMF), `power_restart_leaves_out_what_froze_the_dell` (`acpi.resetsmi` on the command line pretends the register is the SMI port),
`power_restart_that_cannot_restart` (`reboot=none`).

Who may restart or power off the machine: root, and every member of group `shrine` (the people who have a seat at the
machine). Everyone else gets *Operation not permitted* and a machine that keeps running. See [ACCOUNTS.md](ACCOUNTS.md).

### Waking the machine: Wake on LAN, and what woke it (2026-11-02)

**It works on the Dell** (OptiPlex 7050, I219-LM, BIOS: *Wake on LAN* on, *Deep Sleep Control* off): `sudo poweroff`, then `python tools/wol.py aa:bb:cc:dd:ee:ff --to 192.168.1.185` from another PC on the LAN; the Dell
is on the network again about 14 s after the first packet (the firmware takes most of that). `python tools/wake-test.py` runs the whole sequence safely and says what woke the new start. A clock alarm
(`poweroff -w SECONDS`) was built as a fallback and **dropped on 2026-11-03**: the Dell's BIOS did not honour it (try 5 below), Wake on LAN does the job, and the alarm only wrote to the clock chip for no benefit.

Every shutdown path (`reboot(REBOOT_POWEROFF)`, the power button without a desktop, the kernel shell) calls `machine_prepare_poweroff()` (`power.c`) before the last log lines go to the disk, and `machine_poweroff()` finishes the job
as its last steps before `acpi_poweroff()`. Two phases, because what the card logs has to reach the disk, and the card has to stay awake until nothing else needs it. Both run with interrupts off (the network thread polls the cards,
and a poll in the middle of the card's register sequence would be a second user of it).

**Early, once (`machine_prepare_poweroff()`):**

1. *The card is programmed* (`netdev_shutdown_all()` → the driver's `shutdown` hook; `e_shutdown()` in `drivers/e1000.c`). The sequence is the one Intel's I217/I218/I219 documentation and Linux's `e1000e` use for a magic-packet wake, written
   out in the code's own words: the receiver and transmitter are quiesced; `CTRL.ADVD3WUC` is set; for the PCH parts `FEXTNVM6.REQ_PLL_CLK` is cleared, `PHY_CTRL` gets the gigabit-disable bit, and (only when `FEXTNVM` bit 27 says the NVM
   leaves it to software) the PHY's OEM bits get "no gigabit, restart negotiation"; the **PHY's wake-up page** is opened (port-control page 769 register 17: enable access, host wake-up off while writing; page 800 through the address opcode
   0x11 and the data opcode 0x12), the receive address 0, the receive control bits (unicast and multicast promiscuous, multicast offset, broadcast accept, pass MAC control), the wake-up filter control (magic packet) and the wake-up control
   are written, **read back and logged**, and the page is closed with the PHY's host wake-up mode on; the MAC's `WUFC` (magic) and `WUC` (PHY wake, PME enable, APM) are set; `CTRL_EXT.DRV_LOAD` is cleared ("no driver owns the card":
   the management engine takes over); the receiver is switched on again. From here `e_poll` and `e_tx` leave the card alone (`gone`). Plain cards (82540/82574) just get `WUFC`/`WUC`.
   Logged (and so in `TOUHOU.PRV` after the next start): `eth0: wake-up armed: magic packets for ... (WUFC ... WUC ... CTRL ... STATUS ..., RCTL was ...); the card sleeps as the last step`, the `PHY side` line with the read-back
   (on the Dell: receive address 0 `9a50 404c d3fa` valid `8000`, receive control `0020`, WUFC `0002`, WUC `0003`), and the OEM-bits line. `e1000.wol=off` on the command line skips all of it.
2. *The chipset side is prepared* (`acpi_wake_arm()` in `acpi.c`). The I219 is a function of the PCH itself, so its power-management event arrives in the ACPI general-purpose event block as **GPE 0x6D, "PME_B0"** (byte 13, bit 5 of the block
   the FADT's `GPE0_BLK` names; on the Dell io 0x1880, 16 status bytes then 16 enable bytes). The firmware's `_PRW` objects name the wake events; there is no AML interpreter, so the DSDT/SSDTs are scanned for `Name (_PRW, Package () { n, .. })`
   and for the reference firmware's `Return (GPRW (n, ..))`, and the numbers are logged (the Dell's tables name 0x69, 0x08 and 0x6D).

**Last (`machine_poweroff()`, after the disk is flushed):**

3. *The card's second call* (`e_shutdown_last()`): `wake_cause()` reads, logs and clears the cause of any wake-up (the MAC's `WUS`, the PHY's own status on page 800 register 3, the PME status bit), then the power-management control
   register gets PME enable and D3hot.
4. *`acpi_wake_settle()`*: clears the latched `PME_B0_STS` and sets `PME_B0_EN`. Enabling it this late keeps a packet during the shutdown from raising an event nobody handles.

These last lines go to the screen and the serial port, not to the disk (it is flushed already); the next start records them: `acpi: wake status at start: PM1 status ... enable ...; event block ... status ..., enable ...` says what woke it
(a card's PME is byte 13 = `20` of the event block; the firmware clears the power button's PM1 status before the kernel runs) and `wake-up status at start: WUS ... (a magic packet), PHY status ...` is the card's own record.

**The cause of a wake is kept by the card** until software writes it back (the MAC's `WUS`, the PHY's status, the chipset's `PME_B0_STS`), across restarts: the log of the push after the first wake showed `WUS 00000002 (a magic packet)`
and `PHY status 0002` still set. Linux clears both when it resumes; `wake_cause()` does so at start and again at the last step. It was suspected, wrongly, of being the reason for the "instant wake" below.

#### What the Dell did (hardware tries of 2026-11-02, builds `df88d5a`, `76342a4`, `9080d6a`)

| try | what was run | result |
|-----|--------------|--------|
| 1 | `poweroff -w 240`, packets from 25 s after the command for 130 s | the kernel of the next start logged 14 s after the first packet; byte 13 of the event block `20` (PME_B0 latched), PM1 status without the clock alarm: **woken by the packet** |
| 2, 3, 4 | `poweroff -w 90` / `-w 90` / `-w 240` with packets meant to stop "during the shutdown window" (the first 11 s) | back almost at once each time, with PME_B0 latched and `WUS` = magic packet. Read at first as a *latched status that wakes the machine the moment it is off* and answered with two fixes (the settle, the wake-cause clearing). **The reading was wrong**: the machine is off about five seconds after the command (clock chip: command at 20:40:42, next kernel at 20:41:01, of which 14 s is the firmware's), so the packets of the "window" were still arriving after it was off, and woke it, exactly as designed |
| 5 | `poweroff -w 90`, no packets at all (the control) | off for the five minutes the script waited, until the owner pressed the power button: **nothing woke it by itself (so 2 to 4 were the packets), and the clock alarm did not wake it either.** The kernel had set the alarm registers and `PM1_EN.RTC_EN` without error (`timed wake set: clock alarm at ...`); the next start's PM1 status cannot say whether the alarm fired, because the firmware clears it (it did not show the power button either) |

The lesson, and the reason for `tools/wake-test.py`: never send packets less than ~20 s after the poweroff command, and always bound the sender; the machine's own ping log cannot say when it went off (the watcher's first ping after the
command lands seconds later).

Tests: `net_wake_on_lan_armed_at_poweroff` (the PCH path on QEMU's 82574L, the plain path, `e1000.wol=off`), `acpi_tables` (the wake status line).

### Interrupt wiring

The kernel takes its few interrupts (timer, keyboard, mouse, serial) from the legacy 8259 PIC and polls every other
device. Real firmware does not always leave the local APIC in "virtual wire" mode, which is what lets the PIC reach the
boot processor, so `lapic_virtual_wire()` sets LINT0 to ExtINT (unmasked) at boot; it does nothing when the APIC is
globally disabled and understands both xAPIC and x2APIC mode. It also cleans up what the firmware left in the APIC (a raised
task priority, vectors still in service, armed local sources, unmasked I/O APIC inputs) and sends the other processors an INIT
so that they stop running firmware code in memory the kernel is about to reuse.

### The system tick and the TSC

The tick is the 8254 PIT at 1 kHz through the PIC. The TSC (microsecond delays) is calibrated against the PIT's channel 2, with the
ACPI power-management timer and the CPU's brand string ("@ 3.40GHz") as fallbacks and cross-checks; every wait in that code is bounded
because there is no way to see where a PC stopped. If no tick arrives within 200 ms after interrupts are enabled (no 8254, or interrupt
routing the 8259 does not expect), the local APIC timer takes over as the 1 kHz tick (`timer_verify`; reproduce with QEMU
`-machine q35,pit=off`). The boot log says which references were used (`TSC references: ...`).

### DMA below 4 GiB

`dma_alloc()` hands out memory below 4 GiB (`pmm_alloc_below`) even on a machine with 16 GiB of RAM, so controllers
that only do 32-bit addressing work and nothing has to reason about bounce buffers.

## The clock

Time zones follow POSIX: `$TZ`, else the first line of `/etc/timezone`, else UTC. Examples:

```
UTC
JST-9                           (Gensokyo's neighbour: nine hours east, no daylight saving time)
EST5EDT,M3.2.0,M11.1.0          (US Eastern: second Sunday of March to first Sunday of November, 02:00)
CET-1CEST,M3.5.0,M10.5.0/3
AEST-10AEDT,M10.1.0,M4.1.0/3    (southern hemisphere: the summer wraps around the new year)
```

`localtime`, `mktime`, `strftime(%Z %z)`, `date`, `ls -l`, the panel clock and the lock/login screens all follow it.
Set it with `sudo timezone ZONE`: that writes `/etc/timezone` and, with a HUMANVILLAGE volume, keeps it there so it is still set
after a restart ([PERSISTENT_ETC.md](PERSISTENT_ETC.md)); `timezone` alone shows the zone in effect. The default is baked into
the initrd from `initrd/Hakurei/etc/timezone`, and `tz=...` on the kernel command line wins over both (`init` exports it to
every session).

### The clock chip

The kernel reads the battery-backed CMOS clock once at boot and keeps time itself from there. The chip normally holds
UTC, but a PC that also runs Windows keeps *local* time in it. Boot with `rtc=local` and `init` corrects the system clock
by the zone's offset (`tz=` should be set too). Related tools:

* `date [-u] [-d STRING] [-s STRING]` -- `-s` sets the system clock (root only). Strings: `@epoch`, `YYYY-MM-DD`, `YYYY-MM-DD HH:MM[:SS]`.
* `hwclock` -- show the chip. `-s` sets the system clock from it, `-w` writes the system clock to it, `--localtime` says
  the chip holds local time. `/proc/rtc` is the chip's raw value.

Under QEMU the chip starts at the host's UTC time.

**Network time.** Once a network card has an address the kernel sets the system clock from an NTP server (the lease's, `ntp=` on the command line, or a built-in public one) and repeats it every four hours; the clock chip is left alone
unless `ntp.rtc=1` is on the command line (then it is written as `hwclock -w` would). `cat /proc/ntp` shows what happened. [NTP.md](NTP.md) has the details, including how it keeps `init`'s `rtc=local` correction from spoiling a clock that is already right.

## Tests

`tests/case_power.py` (ACPI tables, soft-off, reset, the seat check, the power button with and without a desktop) and
`tests/case_time.py` (zone rules including the exact transition instants, `date -s`, `hwclock`, `tz=` and `rtc=local`).
