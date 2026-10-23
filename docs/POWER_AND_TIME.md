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
| `reboot`, *Restart* | `acpi_reset()`: the FADT reset register (I/O port or PCI config space); then PCI reset control 0xCF9, the keyboard controller pulse, and a triple fault as the last resort |
| the **power button** | a kernel thread polls the PM1 status register (`PWRBTN_STS`, 200 ms). With a desktop running the press arrives as `KEY_POWER`: the window server sends the shell a hotkey and the *Leave the shrine?* dialog opens; a second press powers off. On the login screen one press powers off. Without any window server the kernel powers the machine off itself |

Who may restart or power off the machine: root, and every member of group `shrine` (the people who have a seat at the
machine). Everyone else gets *Operation not permitted* and a machine that keeps running. See [ACCOUNTS.md](ACCOUNTS.md).

### Interrupt wiring

The kernel takes its few interrupts (timer, keyboard, mouse, serial) from the legacy 8259 PIC and polls every other
device. Real firmware does not always leave the local APIC in "virtual wire" mode, which is what lets the PIC reach the
boot processor, so `lapic_virtual_wire()` sets LINT0 to ExtINT (unmasked) at boot; it does nothing when the APIC is
globally disabled and understands both xAPIC and x2APIC mode.

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
Set it permanently by putting a zone in `initrd/Hakurei/etc/timezone` (it is baked into the initrd at build time) or per
boot with `tz=...` on the kernel command line, which `init` exports to every session.

### The clock chip

The kernel reads the battery-backed CMOS clock once at boot and keeps time itself from there. The chip normally holds
UTC, but a PC that also runs Windows keeps *local* time in it. Boot with `rtc=local` and `init` corrects the system clock
by the zone's offset (`tz=` should be set too). Related tools:

* `date [-u] [-d STRING] [-s STRING]` -- `-s` sets the system clock (root only). Strings: `@epoch`, `YYYY-MM-DD`, `YYYY-MM-DD HH:MM[:SS]`.
* `hwclock` -- show the chip. `-s` sets the system clock from it, `-w` writes the system clock to it, `--localtime` says
  the chip holds local time. `/proc/rtc` is the chip's raw value.

Under QEMU the chip starts at the host's UTC time.

## Tests

`tests/case_power.py` (ACPI tables, soft-off, reset, the seat check, the power button with and without a desktop) and
`tests/case_time.py` (zone rules including the exact transition instants, `date -s`, `hwclock`, `tz=` and `rtc=local`).
