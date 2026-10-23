# Real-hardware readiness audit

Everything in this tree had only ever run in QEMU. This page is the result of reading the code the way a Dell OptiPlex-class
desktop would run it: Core i5-7500 (Kaby Lake), 200-series PCH (xHCI `8086:a2af`, AHCI, HD Audio, I219 gigabit Ethernet,
Realtek HDA codec), Intel HD 630 through UEFI GOP, USB keyboard and mouse only, a SATA or NVMe disk behind a PCH root port that
must never be written, booting from a USB stick with Secure Boot off. There is no serial port on that machine, so **the
framebuffer console log is the only diagnostic**: a photograph of the screen has to say where the boot stopped.

How to read the tables: *Evidence* is where in the code the problem lives (or why the code is right), *Risk* is how likely it is
to hurt on the real machine (high = would be visible on the first boot), *Status* is **fixed** (with the test that covers it),
**fine** (read and judged correct against the specification and Linux's behaviour), **open** (not fixed, why), and *Check* is
what to look for on the real machine. The tests are in `tests/case_hwaudit.py` (`python tests/run_all.py hwaudit`).

## What the boot log looks like now

Lines added or changed for this audit, in the order they appear (the tags follow [THEMING.md](THEMING.md)):

```
[Gensokyo] cpu: Intel(R) Core(TM) i5-7500 CPU @ 3.40GHz (GenuineIntel)
[Gensokyo] cpu: family 6 model 0x9e stepping 9: tsc pae apic mtrr pat fxsr sse sse2 sse3 ssse3 sse4.1 sse4.2 xsave avx rdrand smep erms
[Boundary] memory map: 97 regions, 15310 MiB usable + 60 MiB loader + 12 MiB ACPI, 90 MiB reserved, 250 MiB device; RAM reaches 0x47e800000, 12992 MiB of it above 4 GiB
[Kappa] framebuffer 1920x1080 at 0xe0000000, pitch 7680 bytes, write-combining (PAT); the firmware's MTRR for it: UC
[Gensokyo] acpi: rev 2, oem 'DELL  ', 4 processors, PM1a_CNT 0x1804, S5 known, reset register
[Gensokyo] acpi: 27 tables read via the XSDT, PM timer 0x1808
[Gensokyo] local APIC: xAPIC mode, LINT0 was 0x700, now ExtINT (the PIC reaches the boot processor)
[Alice] local APIC: 3 other processors sent to wait-for-SIPI (only the boot processor is used; the spare puppets stay in their box)
[Gensokyo] I/O APIC at 0xfec00000: 120 inputs, 0 had been left unmasked by the firmware
[Sakuya] TSC references: PIT 3407 MHz, ACPI timer 0 MHz, brand string 3400 MHz (0 = unavailable)
[Sakuya] PIT online at 1000 Hz, TSC ~3407 MHz (PIT)
[Kappa] framebuffer speed: a full-screen fill (8100 KiB) took 3.1 ms, ~2600 MB/s
[Marisa] pci: 21 devices found on the bus   ...   [Boundary] reclaimed 61000 KiB of loader and firmware memory
[Marisa] storage probing took 640 ms
[Satori] no PS/2 controller found; keyboard and mouse must come from USB        (or: the PS/2 controller has no mouse on it)
[Gensokyo] boot complete after 4310 ms
```

The numbers above are invented to show the shape of the lines; they were not read from the machine. What matters when the
real machine is photographed:

* `framebuffer speed ... ~N MB/s`: a write-combining mapping fills 1080p in a few milliseconds (hundreds to thousands of MB/s);
  an uncached one needs tens of milliseconds or more. Under 400 MB/s the kernel adds a warning line.
* `no timer interrupt arrived in 200 ms ...` means the PIT's IRQ0 never reached the CPU and the local APIC timer took over
  (the boot continues; this line tells you the chipset routing is different from what the 8259 expects).
* `unexpected interrupt vector N` (at most five lines): something the firmware left armed fired; the kernel acknowledges it.

## 1. Framebuffer performance

A UEFI GOP framebuffer on an Intel iGPU is device memory (the GMADR aperture). Writing it write-combining is fast (a few GB/s),
writing it uncached is a slideshow, and **reading** it in either mode is slow (every load is its own bus round trip).

| # | Finding | Evidence | Risk | Status | Check |
|---|---------|----------|------|--------|-------|
| F1 | The text console scrolled by copying pixels inside the framebuffer, i.e. it READ the whole screen (8 MB at 1080p) for every scrolled line. | `kernel/gfx/fbcon.c` `scroll_up()` called `fb_copy_rect()`, a `memmove` within `g_fb.back == g_fb.front`. | **high**: every scrolled line read 8 MB (1080p) from display memory, and reads of write-combined or uncached memory run at tens to a few hundred MB/s, so each line of the boot log (67 rows per screen, the log is longer than that) and of every later console output would take from tens of milliseconds to a second or more. | **fixed**. `fbcon` keeps the cell grid and a record of what the screen shows (`g_shown`), and repaints only the cells that differ, a whole text row at a time from a RAM buffer, one flush per `write()`; nothing reads the framebuffer. `fbcon_selftest()` (kernel command line `selftest`) reads the screen back after many scrolls, cursor addressing, a disabled period, a 1500-line write and a forced redraw and requires it to equal the grid; `hwaudit_console_scroll_pixels` checks the real console path pixel by pixel against the font. | The boot log scrolls at once, `dmesg` in text mode is instant. |
| F2 | The first pixels were drawn through the loader's direct map, which is **write-back**: a cacheable alias of display memory (writes may sit in the cache; stale lines can later overwrite data written through the write-combining mapping). It also only covered the first `max(4 GiB, RAM)`, so a framebuffer above that would have page-faulted before any output. | `fb_init`: `g_fb.front = phys_to_virt(bf->base)`. | medium | **fixed**. `fb_init` adds a write-combining window of its own (static page tables, up to 64 MiB) to the loader's tables, so even the first pixel is write-combined and any physical address works; one `wbinvd` flushes whatever the firmware cached. `fb_remap` replaces the window with the final ioremap. | `framebuffer ..., write-combining (PAT)` |
| F3 | `wrmsr(IA32_PAT)` ran without checking CPUID.1:EDX[16]. | `kernel/arch/x86_64/cpu.c`. | low (every CPU since the Pentium III has it) but a #GP here is a silent reset. | **fixed**; without PAT the framebuffer (kernel and the window server's mapping) is uncached, `hwaudit_cpu_without_pat` boots a `qemu64,-pat` CPU. | |
| F4 | The window server (`fairyd`) copied the damaged area with `memcpy` = `rep movsb`: on uncached memory that is one bus transaction per byte, on write-combining memory it is fine but not ideal, and there was no `sfence`. | `user/bin/fairyd/comp.c` `flush_rect`. | medium (the "single-digit frames per second" scenario if the mapping were uncached) | **fixed**: 16-byte non-temporal stores (`movntdq`, 4 per iteration = one cache line), sequential, never reading the destination, and an `sfence` after every frame. The kernel's own fills use 8-byte `rep stos/movs` and `sfence`. | |
| F5 | The compositor kept ONE bounding box of damage: a pointer in one corner and a clock tick in the opposite one meant a full-screen compose and a full-screen framebuffer write. | `comp_damage` unioned every rectangle into `S.dirty`. | medium | **fixed**: a list of up to 12 rectangles, merged when they overlap or touch with little waste (`comp_damage` in `comp.c`); `S.dirty` stays the bounding box for the main loop. | |
| F6 | There was no way to see what a frame costs. | | | **fixed**: `/tmp/fairyd.stat` (rewritten every 2 s) shows frames, rectangles, pixels, compose and flush microseconds per frame, the worst frame and the flush speed in MB/s; the kernel times its first full-screen fill and prints the speed (`framebuffer speed: ...`). | `cat /tmp/fairyd.stat` in a terminal window. |
| F7 | **PAT versus MTRR.** The effective memory type combines the PAT entry and the MTRR for the address. Firmware normally leaves the GMADR aperture to the default MTRR type (uncached) or marks it write-combining. As far as the SDM's table for combining the page-level (PAT) and MTRR types says, and as Linux's `ioremap_wc` (which works on machines whose firmware says UC for the aperture) relies on, a PAT entry of WC wins over an MTRR of UC, so the mapping should be write-combining either way. This was not verified against hardware. | cannot be verified in QEMU (it does not model cache types). | medium | **open**: the boot log prints the MTRR type the firmware gave the framebuffer's range (`the firmware's MTRR for it: UC`; QEMU's firmware says UC too) and the measured fill speed is the verification. If it reports a slow framebuffer, boot once with `fb.cache=uc` on the kernel command line and compare: equal speed means the write-combining mapping is not taking effect; the next step would be programming a write-combining MTRR over the aperture (not implemented: it needs the cache-disable / MTRR update dance on every processor). | `framebuffer speed` on both boots. |

Measured in QEMU TCG (a guest framebuffer is plain RAM there, so these numbers only show the instrumentation works and that the
copy method matters when the host emulates instructions one by one): `fairyd` frame flush 584 us with the non-temporal copy and the
rectangle list against 3555 us with `fairyd.copy=rep fairyd.damage=bbox` (the old behaviour; both knobs are read from the kernel command line
through `/proc/cmdline`), 1024x768, a pointer moving over a desktop with one terminal window.

Knobs (all on the kernel command line, i.e. `cmdline.txt` on the stick): `fb.cache=uc`, `fairyd.copy=rep`, `fairyd.damage=bbox`.

## 2. The loader (`boot/`)

| # | Finding | Evidence | Risk | Status | Check |
|---|---------|----------|------|--------|-------|
| L1 | `cmdline.txt` was copied byte for byte. A file saved by a Windows editor or PowerShell (UTF-8 byte order mark, UTF-16, CRLF, several lines, tabs) leaves garbage inside the first word or newlines inside words, and the kernel splits only on spaces: `blk.ro` is not found and **every disk is writable**. | `boot/main.c` (old): `memcpy(cmdline, ...)`. | **high for the safety rule** of this project | **fixed**: `normalize_cmdline` handles all of those (UTF-16 narrowed, control characters and tabs become single spaces, non-ASCII becomes `?`). `hwaudit_cmdline_encodings`. | `loader: cmdline '...'` on the first screen. |
| L2 | A stick without `cmdline.txt` booted with an empty command line, i.e. without `blk.ro`. | | high (same rule) | **fixed**: the safe default is `video=native blk.ro` (`hwaudit_missing_cmdline_means_read_only`). An empty file still means no options (the test harness relies on that). | |
| L3 | Error messages (kernel.elf missing, bad ELF, no boot volume) flashed by for a moment because the loader returned to the firmware, which moved on to the next boot entry. | `return EFI_NOT_FOUND` | medium (no serial port) | **fixed**: the message stays for 30 s (`fail_wait`). Errors after the memory allocations still halt with the message on screen. | |
| L4 | `GetMemoryMap` gets a 256 KiB buffer (5400 descriptors) and the exit was retried 5 times. | | low | **fixed**: a firmware that answers `EFI_BUFFER_TOO_SMALL` gets a bigger buffer, 16 attempts. The retry loop itself (fresh map, then `ExitBootServices` with the new key, nothing in between that allocates) was already right. | |
| L5 | Boot services code/data counted as free memory the moment the kernel started. A device or processor the firmware left running can still be using it: a NIC or controller ring, or a CPU parked in a loop in there. | `classify()` | medium | **fixed**: they are now `BOOTMEM_BOOT_RECLAIMABLE` like the loader's own memory and reach the allocator only after `pci_init` has switched off every DMA master (`pmm_reclaim_boot_memory`), and after the other processors were parked (see 6). ACPI tables the firmware put in such memory are parsed before anything can reuse them. | `reclaimed N KiB of loader and firmware memory` |
| L6 | Memory above 4 GiB and holes. | `build_page_tables` maps `0..max(RAM end, 4 GiB)` (cap 128 GiB) with 2 MiB pages; the kernel builds its own direct map from the RAM-like regions only, so device memory and holes are never mapped write-back. | | **fine**. Tested with 6 GiB (`hwaudit_memory_above_4g`: 4096 MiB above the hole, all of it usable). RAM above 128 GiB would not be reachable before the kernel's own tables exist (irrelevant for 8-32 GiB). | `memory map: ...` line |
| L7 | No fixed physical load addresses, ACPI RSDP, GOP. | The kernel and every buffer come from `AllocatePages(AllocateAnyPages)` and are mapped at `0xFFFFFFFF80000000` by 4 KiB pages; the RSDP search takes the ACPI 2.0 GUID first; the GOP handle is the console's (the display that really has a monitor); `pitch = PixelsPerScanLine * 4` is honoured by kernel and window server; `PixelBitMask` is classified by its masks. | | **fine**. A mode that is not 32-bit linear (`PixelBltOnly`) means no framebuffer at all and a blind console: **open**, nothing to do without a second display path. | `loader: framebuffer WxH at ...` |
| L8 | Reading from a USB stick. | Whole files are read with one `Read()` each (kernel 0.5 MB, initrd 5.8 MB) from the volume the loader came from. | | **fine** (the firmware's FAT and USB mass storage drivers split large requests). `image_usb_stick_boots_desktop` boots exactly that way in QEMU. | |
| L9 | After `ExitBootServices` the loader's messages go to the serial port only. | | low | **fixed differently**: the kernel prints what a photograph needs (memory map summary, framebuffer, ACPI). | |

## 3. xHCI and USB (`kernel/drivers/usb/`)

Read against the xHCI specification 1.1/1.2 and Linux's `drivers/usb/host/xhci*.c`. **No defect found**; the driver was clearly
written for real silicon first. Verified, with the function that does it:

* BIOS-to-OS handoff through the USB Legacy Support capability before the controller is touched (`bios_handoff`: OS-owned semaphore,
  1 s wait, SMI enables cleared, status acknowledged), after the PCI function was brought to D0 (`xhci_create`).
* Controller reset with timeouts: wait `CNR`, halt, `HCRST` (2 ms pause on Intel), wait `HCRST` and `CNR` (`x_halt_reset`).
* Scratchpad buffers from both halves of HCSPARAMS2 (`Max Scratchpad Bufs Hi/Lo`), array in `DCBAA[0]`, every page 4 KiB aligned.
* 32- and 64-byte contexts (`HCCPARAMS1.CSZ`): every context address uses `csz`; the 64-byte variant has no QEMU model, so
  it is **untested** (**check** on the machine: `lsusb`/log line says `32-byte contexts` or `64-byte contexts`).
* Alignment and boundaries: every ring, ERST, DCBAA, context and scratchpad page comes from `dma_alloc` (page aligned, below 4 GiB),
  so no 64-byte segment can cross a 64 KiB boundary; bulk buffers use a 64 KiB aligned bounce.
* Link TRB handling and cycle toggling (`ring_push`), event ring dequeue pointer with `EHB` (`ring_update_erdp`), 64-bit registers
  written low dword first.
* Supported Protocol capability: which ports are USB 2 or 3, protocol slot type, speed ID tables (`parse_ext_caps`).
* Port power, USB 3 link states, warm reset for ports stuck in Polling/Inactive/compliance, a second look at all ports after 2 s
  (`late_port_scan`), hot-plug through Port Status Change events.
* Address Device with BSR first and again without, `bMaxPacketSize0` fix through Evaluate Context, four attempts with a fresh reset, old
  order on the later attempts (`dev_address`), 10 ms after SET_ADDRESS, three tries for every descriptor read.
* Endpoint intervals for low/full/high/super speed as Linux converts them (`ep_interval`), Max ESIT payload set for interrupt
  endpoints.
* Hubs with transaction translators (single and multiple TT), USB 3 hubs with SET_HUB_DEPTH, hub slot context update in Linux's order.
* HID boot keyboards: `SET_PROTOCOL(boot)` and `SET_IDLE(0)`, neither fatal if the device stalls; LED report; mice by report descriptor.
* Every wait is bounded and a controller that stops answering is switched off with one log line (`x_die`).

Check on the machine: `[Kappa] xhci 00:14.0: took the controller over from the firmware` (or nothing, if the firmware had already
released it), the `xHCI 8086:a2af ... ports (N usb2, M usb3) ...` line, `N of M ports have something attached: ...`, then
`usb 1-X: new ... device` and `keyboard online` / `mouse online`. If no keyboard is bound 8 seconds after boot the kernel prints the
raw PORTSC of every port (`no USB keyboard is bound 8 s after boot; ...`). `usb=debug` on the command line logs every step.
Use a rear port directly on the board for the first try (many Dell keyboards contain a hub; hubs are supported, but fewer layers
is easier to read).

## 4. PCI

| # | Finding | Evidence | Risk | Status |
|---|---------|----------|------|--------|
| P1 | Enumeration starts at bus 0 and follows PCI-to-PCI bridges through the secondary bus numbers the firmware assigned, depth 8. | `scan_bus`/`scan_function` | | **fine** on a UEFI desktop (firmware assigns bus numbers and BARs for everything it enabled, including the root port in front of an NVMe drive). A bridge with secondary bus 0 is skipped. |
| P2 | Configuration access is the legacy `0xCF8/0xCFC` mechanism: no ECAM/MCFG, so config space above 0xFF (PCIe extended capabilities: AER, L1 substates, SR-IOV) is unreachable. | `cfg_read32` | low | **open**: no driver needs extended capabilities; adding MCFG is straightforward when one does. |
| P3 | BAR sizing switches the device's memory decoding off and writes all ones into the BARs. For the display adapter that is the window the console is drawing through. | `probe_bars` | low-medium | **fixed**: class 03 devices are not sized at all (Linux's `mmio_always_on`). |
| P4 | BARs: 64-bit and prefetchable BARs are sized correctly (`type == 2` takes two registers), memory and I/O decoding are restored. A BAR the firmware left unassigned (base 0) makes `pci_map_bar` return NULL and the driver logs `has no memory BAR`. | | | **fine** |
| P5 | Driver matching is by class code (`pci_find_class`): xHCI `0C/03/30`, AHCI `01/06/01`, NVMe `01/08/02`, HDA `04/03`, Ethernet by vendor/device id only for the Intel/virtio families that are known. | | | **fine** |
| P6 | The kernel assigns no resources: bridge windows and BARs are whatever the firmware programmed. A device the BIOS left disabled has base 0 and is skipped by `pci_map_bar` (its driver logs it); nothing is ever moved. | `probe_bars`, `pci_map_bar` | | **fine** for a PC that boots from UEFI (it enumerates and assigns everything it enabled, including the ranges behind the root port of an NVMe drive). |
| P7 | `quiesce_dma` clears bus mastering on storage, network, audio and USB endpoints at scan time so firmware leftovers cannot DMA into reused memory (bridges and the display adapter are left alone on purpose); each driver turns it back on when it claims the device. | | | **fine** |

## 5. AHCI and NVMe

Read against AHCI 1.3.1 and NVMe 1.4 and Linux's `libahci`/`nvme-pci`. **No defect found.**

* AHCI: `BOHC` BIOS/OS handoff (`hba_handoff`), only ports in `PI` and inside the register window, stop engine/FIS with the 500 ms rule,
  `SUD`/`POD` power-up, `COMRESET` with a 200 ms settle and a 100 ms stable-link requirement, link power management switched off, `CAP.S64A`
  respected (and everything the driver allocates is below 4 GiB anyway), command list 1 KiB / FIS 256 B / command table 128 B alignment
  inside one page-aligned block, PRDT entries up to 4 MiB, 512/512e/4Kn sectors parsed from IDENTIFY (with a built-in self test, `ahci.selftest`),
  every wait bounded, a failed command resets the link and the port is declared dead if it does not come back, Intel RST "RAID On"
  mode is recognised and explained (`SATA controller is in RAID (Intel RST) mode`).
* NVMe: `CAP.TO` (500 ms units) for every enable/disable handshake, `MQES` clamps the queue depth, `DSTRD` for the doorbells, `MPSMIN`
  must be 0 (4 KiB pages), `CSS` must include the NVM command set, active namespace list (`CNS 2`) instead of assuming namespace 1, MDTS
  limits the transfer size, PRP lists for transfers beyond two pages, phase tag wrap-around, timeouts everywhere, a controller that sets
  `CSTS.CFS` is switched off with bus mastering disabled.
* Safety: `blk.ro` makes both drivers register read-only devices. IDENTIFY and the partition scan only read. Neither driver writes
  to a disk unless something above the block layer writes (the HUMANVILLAGE mount policy, `dd` by root). The NVMe driver resets the
  *controller* (`CC.EN` 0 then 1) as part of taking it over, which does not touch the media.
* Minor, **open**: the admin queue is 16 entries and is not clamped to `CAP.MQES` (every real controller has MQES >= 63).

## 6. CPU, timers, interrupts

| # | Finding | Evidence | Risk | Status |
|---|---------|----------|------|--------|
| C1 | **The TSC calibration loop could spin forever** (`while (!(inb(0x61) & 0x20))` with no bound) on a chipset where the PIT's channel 2 does not behave, which is a silent hang before the first line about the timer. | `timer.c` `calibrate_tsc` | medium | **fixed**: bounded, with the ACPI PM timer as the second reference and the "@ 3.40GHz" of the brand string as the third; the three are cross-checked (25 %), `TSC references: ...` shows all of them. `hwaudit_pit_missing`. |
| C2 | **If the PIT's IRQ0 never reaches the CPU the boot froze at the first sleep** (everything with a timeout then loops forever because `uptime_ms()` never advances): a chipset without the legacy timer, HPET in legacy-replacement mode, an interrupt route the firmware changed. | QEMU `-machine q35,pit=off` reproduces it: the old kernel stopped after the `ahci0` lines. | medium | **fixed**: `timer_verify()` waits 200 ms for a tick and otherwise starts the local APIC timer (calibrated against the TSC) as the 1 kHz tick (vector `0x40`), masking IRQ0. |
| C3 | Local APIC state left by the firmware: raised task priority, vectors still in service, armed LVT sources (timer, thermal, performance counters, CMCI, error), spurious vector. Any unacknowledged vector at 48 or above blocks the PIC's vectors 32..47 (the system timer, keyboard) for good. | `lapic.c` | low-medium | **fixed**: TPR/CR8 = 0, in-service vectors acknowledged, the unused LVT entries masked, spurious vector 0xFF; a stray vector gets its end-of-interrupt and a rate-limited log line. |
| C4 | The other processors were left where the firmware parked them, which may be a loop in boot services memory that the allocator now reuses; a core that executes garbage triple-faults and an Intel chipset resets the whole machine, seconds after the screen came up. | | medium | **fixed**: INIT to all other processors (SDM 8.4.4.1) puts them in wait-for-SIPI (`hwaudit_other_processors_parked`, QEMU `-smp 4`); firmware memory is also reclaimed late (L5). Real multiprocessor start-up is not implemented (`docs/STATUS.md`). |
| C5 | The i8042 probe: a controller that exists but has no mouse attached made every one of about 15 mouse commands wait 100000 port reads (seconds, on slow legacy I/O); a missing controller that does not read as all ones likewise. | `keyboard.c` | medium (seconds of black screen) | **fixed**: waits are bounded in time (25 ms), the first unanswered command ends the probe, a missing keyboard or mouse is reported (`the PS/2 controller has no mouse on it`), and no interrupt handler is installed for a device that is not there. `hwaudit_no_ps2_controller` covers the floating-bus case. |
| C6 | CPU features: SMEP and RDRAND are used only when CPUID reports them, PAT now too; SSE state is set up explicitly (`fninit`, `ldmxcsr 0x1F80`, CR0.NE/MP, CR4.OSFXSR/OSXMMEXCPT) instead of trusting the firmware; `fxsave`/`fxrstor` on task switch; no AVX anywhere (userland is plain x86-64); SYSCALL MSRs set. Under `-cpu max` and under `Skylake-Client-noTSX-IBRS` the same code runs. | `cpu.c`, `syscalls.c` | | **fixed/fine** |
| C7 | RTC: update-in-progress handled, century register, 12/24-hour and BCD/binary; a dead RTC gives a wrong but harmless date. i8042, serial: absent hardware is detected and costs no time; the serial port is probed with a scratch register. | `rtc.c`, `serial.c` | | **fine** |
| C8 | ACPI parsing: every table is length-checked (at most 16 MiB), signatures checked, entries above 4 GiB are mapped through ioremap when they are not in the direct map, pointers above 2^48 are refused, checksums are counted and logged but the tables are still used (as Linux does); `_S5` is found by scanning the DSDT/SSDTs; S5 poweroff handles `SCI_EN` through `SMI_CMD`; a failed poweroff now says so on screen. | `acpi.c`, `power.c` | | **fixed/fine** |
| C9 | The power button is polled through the fixed-hardware PM1 status bit; machines that describe it as a control method (FADT flag) need an AML interpreter and the button does nothing (`/proc/acpi`: `button: control method (not handled)`). | | | **open**: the desktop's own power menu works, holding the physical button 4 s powers off in hardware. |
| C10 | Interrupt sources the kernel does not own: I/O APIC redirection entries the firmware left unmasked would deliver at vectors this kernel does not expect (or deliver a device's interrupt a second time next to the 8259). | | low | **fixed**: every I/O APIC in the MADT gets all inputs masked at start-up (`I/O APIC at ...: N inputs, M had been left unmasked`); a stray vector that still arrives is acknowledged and logged (C3). The kernel keeps using the 8259 through LINT0 (virtual wire, the configuration Linux ends up in with `noapic`); if that route did not work on the machine, the missing tick is caught by C2. |
| C11 | VT-d: the kernel assumes no DMA remapping is active (true unless the firmware enabled pre-boot DMA protection and left it on). | | low | **open**: would show as DMA that never completes (the xHCI event ring stays empty, `no USB keyboard is bound 8 s after boot`); check the "Pre-Boot DMA Protection"/VT-d BIOS option. |

## 7. Memory manager on 8-32 GiB with holes

* The page database costs 16 bytes per 4 KiB frame (128 MiB at 32 GiB) and is carved from the first USABLE region larger than twice its
  size; a failure panics with the number (`pmm: no region large enough for the page database`). The direct map is built from 2 MiB pages per region
  (4 KiB at unaligned edges), which takes microseconds. The region table holds 512 entries (it was 256; truncation is now logged).
* The kernel heap is the buddy allocator (blocks up to 16 MiB); there is no separate heap limit.
* DMA buffers come from below 4 GiB (`pmm_alloc_below`), whatever the controller's addressing; the page database and everything else can
  live above it.
* Tested: 512 MiB, 768 MiB and 6 GiB (4 GiB above the PCI hole). Not tested: 32 GiB (QEMU would need 32 GiB of host memory).

## 8. Other things found while reading

* `panic()` and the exception path printed to a console that the window server had switched off (it owns the screen), so a panic on the desktop would
  show a frozen desktop. **Fixed**: `fbcon_force_on()` takes the screen back first (`fbcon_selftest` covers the path).
* A failed power-off (S5 not working) halted silently. **Fixed**: it says so and tells you to hold the button.
* The kernel log scrolled through a path that was slow on write-combining memory (F1).
* `kernel/drivers/usb/usb.c` prints raw PORTSC values when no keyboard shows up: keep this when changing the driver.
* **A test flake that is not a kernel bug** (found while running the suite for this audit): `utest` and the other tests that end through
  `qemu_exit()` sometimes fail with `QEMU exited (code 1) while waiting for /UTEST (PASS|FAIL)/` and a serial log that stops in the middle of
  the last lines. Exit status 1 is the guest saying *success* (`(0 << 1) | 1` from `isa-debug-exit`); the trace of such a run shows a healthy
  guest reaching `init`'s `reboot()` (see `-d int` log tails). The harness reads the guest's serial port over TCP, QEMU leaves through `exit()`,
  and on a loaded Windows host the connection is reset before the reader thread has fetched the last bytes. It happens at the same rate on
  `main` (2 of 120 runs each in a side-by-side loop, `utest` boots with other test runs going on) and on this branch (2 of 120), so it is not
  caused by the changes here. `qemu_exit()` now waits 300 ms before leaving so that the reader can catch up (0 failures in 150 runs afterwards,
  under lighter load: not a clean comparison). Not a hardware matter, noted so nobody chases it.

### Network and audio drivers on main (noted, not changed; their owners review the details)

* `e1000.c` (I219 family on the target): the PCH parts get no global reset and no PHY access, by design; `pci_enable` does not check or clear the
  PCI power state first (the xHCI driver does), which matters if the BIOS leaves the LAN powered down. The MAC address is taken from RAL/RAH; the
  link may stay down without a driver-side PHY/ME handshake: the driver logs and leaves the card alone, boot is not affected.
* `audio/hda.c`: matches by class (`04/03`), all waits are bounded (reset 100-200 ms, codec wait up to 500 ms, stream resets 5-20 ms), so a codec that does not
  answer costs less than a second of boot.

## Checklist for the first boot on the real machine

1. Photograph the screen at three moments: right after the loader (first screen), when the log stops scrolling, and with the desktop up.
2. Read: CPU line, `memory map`, `framebuffer ... write-combining`, `framebuffer speed` (should be hundreds of MB/s or more), `acpi: ...`,
   `TSC references`, `pci: N devices`, `xhci ...`, `ahci0 ...`/`nvme0 ...` (and that `blk.ro` is in the `cmdline` shown in the `image` line).
   Once the log has scrolled away (or on the desktop, in a terminal window):
   `dmesg | grep -E "cpu:|memory map|framebuffer|TSC|acpi|APIC|xhci|ahci|nvme|storage|boot complete"`.
3. If the log scrolls slowly: `fb.cache=uc` once, compare the two `framebuffer speed` lines (F7).
4. If the keyboard does not work: `[Satori] no USB keyboard is bound 8 s after boot; xhci ... ports (number:PORTSC): ...` is the line to photograph.
5. On the desktop: `cat /tmp/fairyd.stat` in a terminal; compose and flush microseconds per frame, and the flush speed.
6. If the machine resets by itself a few seconds after the picture appears: suspect memory the firmware still used (L5/C4 should have made that
   impossible) or an unhandled interrupt source (C10).
7. If the screen was not photographed in time (or the machine froze): pull the stick and read `TOUHOU.LOG` (this boot) and `TOUHOU.PRV` (the boot
   before) in the root of its big partition on another computer, see [REAL_HARDWARE.md](REAL_HARDWARE.md). They hold the same lines as the screen,
   the audit lines above included, up to about five seconds before the end; a file without the "clean run" line at its end belongs to a boot that did not finish.

## Tests

`python tests/run_all.py hwaudit` runs the audit tests: PIT missing, other processors parked, a CPU without PAT, `fb.cache=uc`, no PS/2 controller,
6 GiB of RAM, cmdline.txt encodings, missing cmdline.txt, the fbcon self test, the pixel-exact scroll check and fairyd's statistics and knobs. The
`selftest` test includes the fbcon check. `tools/qemu.py` honours `TOUHOU_CPU=Skylake-Client-noTSX-IBRS` (set by another branch) if you want
the guest to look like the target's CPU family.
