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

## The first real boot (2026-10-23): what the Dell taught us

The first image stopped on the line `loader: leaving boot services` and never reached the kernel: **the firmware's `ExitBootServices` call did not
return**. QEMU/OVMF never showed it. How it was found, in one evening, with no serial port: squares drawn straight into the framebuffer at each
stage (`shared/stagemark.h`: they showed that the first attempt of the exit call started and never came back), a restart probe (`exitreset`: the
machine did not restart by itself, so the CPU was stuck inside the call), and a *ladder* (`ladder`, `strategy=N`) that tries one set of
workarounds per boot, counted in an NVRAM variable named after the build.

The rungs, from `boot/main.c`: 0 restart probe, 1 no loader writes to the screen after the exit call, **2 ordinary memory types for the kernel and the
initrd (the fix)**, 3 plus interrupts off and no console text before the call, 4 plus no loader squares, 5 three seconds of patience, 6 the firmware's USB
drivers released first, 7 everything, 8 no exit call at all, 9 that without interrupts and squares. Rung 2 booted to the desktop; rung 1 had not.

Cause: the loader allocated the kernel image and the initrd with OS-defined UEFI memory types (`0x80000001`/`0x80000002`, which the specification
reserves for OS loaders). On this firmware a memory map with those types makes the exit call hang. Now the kernel and the initrd are plain
`EfiLoaderData`; the loader gives the kernel the same information by retyping those two ranges in the map it hands over (`BOOTMEM_KERNEL`,
`BOOTMEM_MODULE`; `retype_range`/`sort_merge`), and writes nothing to the screen once the exit call has returned. The old behaviour is still there
as the words `customtypes` and `exitmarks` for experiments. Lesson for every other loader: do not use OS-defined memory types.

The debug words, all handled by the loader and (where it makes sense) the kernel: `stagehold=N` (stop after square N), `stagebeep` (N beeps for stage N,
needs a PC speaker, this Dell has one), `exitreset`, `noexit`, `cliexit`, `quietexit`, `nomarks`, `stall3`, `usbdisc`, `ladder`, `ladderstart=N`, `strategy=N`.

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
[Kappa] fb.mtrr=auto: the screen takes 2600 MB/s, fast enough: the MTRRs stay as the firmware set them
[Marisa] storage probing took 640 ms
[Satori] no PS/2 controller found; keyboard and mouse must come from USB        (or: the PS/2 controller has no mouse on it)
[Gensokyo] boot complete after 4310 ms
```

The numbers above are invented to show the shape of the lines; they were not read from the machine. What matters when the
real machine is photographed:

* `framebuffer speed ... ~N MB/s`: a write-combining mapping fills 1080p in a few milliseconds (hundreds to thousands of MB/s);
  an uncached one needs tens of milliseconds or more. Under 400 MB/s the kernel adds a warning line, and after the PCI scan it corrects
  itself if it can (section 1, F7): `mtrr: ...` (the firmware's layout), `framebuffer 0x... lies in a prefetchable BAR of ...`, and
  one line with the verdict, either `framebuffer: write-combining MTRR #N over ...: fb speed before N MB/s after M MB/s, kept` (or `no gain:
  the MTRR is removed again`) or `framebuffer: no MTRR added (reason); fb speed N MB/s stays`.
* `[Marisa] pci 00:xx.x ... was asleep (power state D3): moved to D0` appears only if the firmware left a function in a low-power state that a
  driver then claimed (P9).
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
| F7 | **PAT versus MTRR.** The effective memory type combines the PAT entry of the page and the MTRR type of the address. Re-read for this round (Intel SDM vol. 3A, "Effective Page-Level Memory Types for Pentium III and More Recent Processor Families", as Xen's `mm_type_tbl` and Linux's `ioremap_wc` use it; AMD's table says the same for WC): **a PAT entry of WC gives write-combining whatever the MTRR says, UC included**, so a firmware MTRR of UC over the aperture (QEMU's OVMF and most PCs: default UC, or a UC range for the PCI hole) does not by itself call for anything. The other direction does not work: an MTRR of type WC does nothing for a page whose PAT type is UC (only for UC-, the `PTE_PCD`-alone encoding), a UC MTRR range always wins over a WC one, and WB over WC is undefined. So the MTRR is a second route, useful only if the PAT route measures slow. | cannot be verified in QEMU (it does not model cache types, only the registers). | medium | **fixed as a measured fallback**, see F8. The boot log still prints the MTRR type the firmware gave the framebuffer's range (`the firmware's MTRR for it: UC`) next to the measured speed. | `framebuffer speed`, `fb.mtrr=auto: ...` |
| F8 | **No second route when the screen is slow.** The owner can boot the real machine only by hand, one boot at a time; a console that crawls (write-combining not in effect for any reason) was the likeliest way for that boot to go wrong, and nothing corrected it. | `gfx/fbtune.c`, `arch/x86_64/mtrr.c` | medium | **fixed**: `fb_tune()` runs after the PCI scan. It times a fill of the bottom text rows (at least 512 KiB, best of three, repainted afterwards); under 400 MB/s (`fb.mtrr=on`: always) it prints the firmware's MTRR layout, finds the memory BAR of the display adapter that holds the framebuffer (sized on demand with decoding left on, as Linux does for the boot display; must be prefetchable and contain no RAM), asks `mtrr_plan_wc` for a free variable register with **no overlapping range of any other type** (UC wins over WC, WB/WT/WP over WC is undefined, so such layouts are refused with the reason on screen), writes one WC range over the whole BAR with the SDM's update sequence (interrupts off, CD=1, `wbinvd`, ranges off, write the pair, ranges on, flush again, CD=0; the other processors are already parked; the values are read back), switches the framebuffer pages to UC- (`PTE_PCD` alone, for the kernel and for later `/dev/fb0` mappings), measures again, logs `fb speed before N MB/s after M MB/s`, and **removes the range and restores the page flags unless the screen got 1.5 times faster** (`fb.mtrr=on`: unless it got clearly slower). `fb.mtrr=off` never touches the MTRRs; `fb.cache=uc` disables all of it. A future SMP start must copy the boot processor's MTRRs into each core it wakes. Tests: the kernel self test runs the decision function over made-up firmware layouts (OVMF's own, a UC range over the aperture, a WB range over it, a partial overlap, no free register, already WC, a mask with holes, MTRRs off, no WC type, bad windows, above the address width), the real update sequence on QEMU's emulated MSRs (CR0, CR4 and the default-type register compared before and after) and the whole attempt with scripted speeds (kept, no gain, forced, loss) with the framebuffer's PTE flags checked each time (`hwaudit_mtrr_selftest`); `hwaudit_mtrr_forced_attempt` boots with `fb.mtrr=on` (OVMF puts a UC range over its aperture, so the log must say why no MTRR was added, with the BAR found by sizing) and with a CPU without MTRRs; `hwaudit_mtrr_knob_off_and_auto`. | `mtrr: ...`, `framebuffer ... lies in a prefetchable BAR`, `fb speed before N MB/s after M MB/s` (only when the screen was slow). |

Measured in QEMU TCG (a guest framebuffer is plain RAM there, so these numbers only show the instrumentation works and that the
copy method matters when the host emulates instructions one by one): `fairyd` frame flush 584 us with the non-temporal copy and the
rectangle list against 3555 us with `fairyd.copy=rep fairyd.damage=bbox` (the old behaviour; both knobs are read from the kernel command line
through `/proc/cmdline`), 1024x768, a pointer moving over a desktop with one terminal window.

Knobs (all on the kernel command line, i.e. `cmdline.txt` on the stick): `fb.cache=uc`, `fb.mtrr=off|on|auto`, `fairyd.copy=rep`, `fairyd.damage=bbox`.

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

The only change this audit made in `kernel/drivers/usb/` is one line in `xhci.c`: its private D0 wake-up in `xhci_create` is gone, `pci_enable` does it for
every driver now and puts the BARs back after a soft reset (P9). The USB mass storage driver (`usb/storage.c`) and the log on the stick were merged from main after
the first review; the second review is the subsection below.

Check on the machine: `[Kappa] xhci 00:14.0: took the controller over from the firmware` (or nothing, if the firmware had already
released it), the `xHCI 8086:a2af ... ports (N usb2, M usb3) ...` line, `N of M ports have something attached: ...`, then
`usb 1-X: new ... device` and `keyboard online` / `mouse online`. If no keyboard is bound 8 seconds after boot the kernel prints the
raw PORTSC of every port (`no USB keyboard is bound 8 s after boot; ...`). `usb=debug` on the command line logs every step.
Use a rear port directly on the board for the first try (many Dell keyboards contain a hub; hubs are supported, but fewer layers
is easier to read).

### Booting from a stick: the sequence, and `storage.c`, `mountpolicy.c`, `bootlog.c` against what real sticks do

**The sequence** (read, judged right; nothing changed): the firmware's xHCI driver reads `kernel.elf` from the stick and halts the controller at
`ExitBootServices` (EDK2's `XhciDxe` does `XhcHaltHC` and clears the BIOS-owned flag). Then `bios_handoff` (only if the firmware still claims the
controller), `x_halt_reset` (`HCRST`), the ports re-detect their devices and queue Port Status Change events, `port_connect` debounces for 100 ms, resets
the port (a warm reset for a USB 3 link that does not train; a stick whose SuperSpeed link never trains shows up on the companion USB 2 port of the same
connector), addresses the device with BSR first, reads descriptors with three tries each and repeats everything with a fresh reset up to four times.
The port reset is a bus reset for the stick, so the address and configuration the firmware gave it are forgotten before we address it. The boot waits
at most 8 s for the bus (`usb_wait_settled`); a stick that needs longer is mounted later by the hot-plug hook (`usbstor_late_ready`).

**Against real sticks** (Linux's `usb-storage` and its unusual-devices list are the catalogue). Status "WIP" means: designed and at least partly written,
not finished, not built, not tested, parked on the branch `wip/usb-storage-soft-fallbacks` ("State when paused" below says which parts exist); the
audit branch's `storage.c` is still the merged original, so none of the fixes in this table is in effect yet.

| # | What real sticks do | What `storage.c` does today (evidence) | Status |
|---|---------------------|----------------------------------------|--------|
| U1 | Do not answer GET MAX LUN at all: the control request times out (Linux takes one LUN). | `msc_probe`: `rc == -ETIMEDOUT` returns `-ENODEV`; the stick is not driven. | WIP: clear `dev->broken`, one LUN |
| U2 | Hang on MODE SENSE (the write-protect probe). | `lun_write_protected` says "writable" but the timeout left `dev->broken` set: every later transfer fails with `-ENODEV`, the disk registers and is useless. | WIP: reset recovery, writable, never asked again |
| U3 | Put nonsense in the status wrapper's residue: more than was asked for, or "nothing moved" after a read that went through (Linux: `US_FL_IGNORE_RESIDUE`, and it clamps). | `csw_check` turned a residue above the request into a phase error (reset recovery, retry: a stick that always does it never becomes ready); `msc_io_one` failed a good read whose residue was not 0. | WIP: the bytes that moved and the status byte decide, residue clamped and counted |
| U4 | Hang or garble transfers above 32 KiB (Linux caps some at 32 KiB by quirk entry). | One size, 64 KiB; a hang ends in `msc_died`, the stick is gone. | WIP: ladder 64, 32, 16, 4 KiB: a hang resets the stick and goes down at once, a second failure of a command goes down; logged |
| U5 | Need more than 400 ms after SET_CONFIGURATION (Linux waits 1 s). | `FIRST_COMMAND_DELAY_MS 400`. | WIP: 1000 |
| U6 | Hang once on INQUIRY, TEST UNIT READY or READ CAPACITY while still waking up. | `msc_died` at the first timeout. | WIP: `msc_run_boot`: up to two resets and retries per stick |
| U7 | READ CAPACITY(10) says "more than 2 TiB" (0xFFFFFFFF) on a bridge that cannot do (16); block size 0. | Both end with the stick ignored. | WIP: 2^32 blocks like Linux; block size 0 means 512 |
| U8 | The last block is off by one (`US_FL_FIX_CAPACITY`). | Trusted. | WIP: quirk `fixcap` only; no probing read (reading the last block of a flaky stick can itself hang it) |
| U9 | Individual behaviour that cannot be guessed. | No table. | WIP: `g_quirk_table` (empty) and `usbstorage.quirk=VID:PID:flag,flag/...` (flags `nosync nowp fixcap chunk32k chunk16k chunk4k slowstart`) |
| U10 | A stick that stalls or ignores the bulk-only reset itself: Linux falls back to a port reset (`usb_reset_device`) and re-enumerates it in place. | `msc_garbage` gives the stick up (`-ENODEV`). | **not done**: needs a re-enumerate-in-place facility in `usb.c`/`xhci.c` (Reset Device command or port reset, Address Device, Configure Endpoint again) |
| U11 | Diagnostics that fit a photograph. | Events are logged (stick with vendor:product, size, writable, "given up on", ...); a failing single command was only in `usb=debug`. | WIP: `msc_note` (the first 8 problems per stick on the screen, with opcode, LBA, sense key/asc/ascq). **Not done**: one `usb: summary` line before "boot complete" (devices with their drivers, LUN states, HUMANVILLAGE result) |
| U12 | Sticks and enclosures that offer both UAS and bulk-only. | Only alternate setting 0 of an interface is parsed, so bulk-only is used when it is alt 0; a device whose alt 0 is UAS is logged and ignored (`usbstor_uas_ignored`). | **fine** |
| U13 | `mountpolicy.c`, `bootlog.c`. | Boot sectors and labels are read, nothing else; one HUMANVILLAGE volume, the first found, internal disks before sticks; the log is written only to a clean, writable volume, synced after every write, rotated to `TOUHOU.PRV`; a stick pulled mid-run costs `-ENODEV`, not a hang. | **fine**; the FAT dirty-volume work in `fat*.c` and `mountpolicy.c` belongs to the USB agent and was not touched |

## 4. PCI

| # | Finding | Evidence | Risk | Status |
|---|---------|----------|------|--------|
| P1 | Enumeration starts at bus 0 and follows PCI-to-PCI bridges through the secondary bus numbers the firmware assigned, depth 8. | `scan_bus`/`scan_function` | | **fine** on a UEFI desktop (firmware assigns bus numbers and BARs for everything it enabled, including the root port in front of an NVMe drive). A bridge with secondary bus 0 is skipped. |
| P2 | Configuration access is the legacy `0xCF8/0xCFC` mechanism: no ECAM/MCFG, so config space above 0xFF (PCIe extended capabilities: AER, L1 substates, SR-IOV) is unreachable. | `cfg_read32` | low | **open**: no driver needs extended capabilities; adding MCFG is straightforward when one does. |
| P3 | BAR sizing switches the device's memory decoding off and writes all ones into the BARs. For the display adapter that is the window the console is drawing through. | `probe_bars` | low-medium | **fixed**: class 03 devices are not sized at scan time (Linux's `mmio_always_on`); see P10 for the one case where it is sized later. |
| P4 | BARs: 64-bit and prefetchable BARs are read as two registers where `type == 2` (and sized by the lowest-set-bit rule, see P8), memory and I/O decoding are restored. A BAR the firmware left unassigned (base 0) makes `pci_map_bar` return NULL and the driver logs `has no memory BAR`. | | | **fine** |
| P5 | Driver matching is by class code (`pci_find_class`): xHCI `0C/03/30`, AHCI `01/06/01`, NVMe `01/08/02`, HDA `04/03`, Ethernet by vendor/device id only for the Intel/virtio families that are known. | | | **fine** |
| P6 | The kernel assigns no resources: bridge windows and BARs are whatever the firmware programmed. A device the BIOS left disabled has base 0 and is skipped by `pci_map_bar` (its driver logs it); nothing is ever moved. | `probe_bars`, `pci_map_bar` | | **fine** for a PC that boots from UEFI (it enumerates and assigns everything it enabled, including the ranges behind the root port of an NVMe drive). |
| P7 | `quiesce_dma` clears bus mastering on storage, network, audio and USB endpoints at scan time so firmware leftovers cannot DMA into reused memory (bridges and the display adapter are left alone on purpose); each driver turns it back on when it claims the device. | | | **fine** |
| P8 | **BAR sizes were computed as `~mask + 1`** from the value read back after writing all ones. That is only right when every address bit above the BAR's size reads back as one. A 64-bit BAR of a device with a 40- or 48-bit address width reads zeros in the upper bits (so does a small 32-bit device that implements fewer bits), and the result was a size of exabytes: `pci_map_bar` would then `ioremap` that, a loop over about 2^47 pages that never comes back. (Found while writing P9's code; the first review of this file called BAR sizing "fine".) | `probe_bars` | **medium-high**: an xHCI, NVMe or HDA BAR0 of that kind would hang the boot at the first `pci_map_bar`, with the last log line being the driver's probe line. | **fixed**: the size is the lowest address bit that reads back as one (`bar_size`, what Linux's `pci_size` does). Tested with the awkward read-backs (`hwaudit_pci_selftest`). |
| P9 | A function the firmware (or a previous OS) left in D1-D3hot only answers its power-management registers. The xHCI driver woke its own function; the others (e1000e/I219, HDA, AHCI, NVMe, the other NICs) did not. | `pci_enable` | low-medium (the I219 on an unused-LAN BIOS setting, an HDA function the BIOS turned off) | **fixed**: `pci_enable` calls `pci_set_d0` first: PMCSR to D0 (PME_Status not cleared by accident), 10 ms after D3hot (1 ms after D1/D2), and unless the NoSoftReset bit says otherwise the BARs and the command register are written back, as Linux's `pci_restore_bars` does. `xhci.c`'s private copy is gone. Logged: `pci 00:1f.3 ... was asleep (power state D3): moved to D0`, or `stays in power state D3 after being told to wake up`. `hwaudit_pci_selftest` puts an emulated e1000e into D3hot and checks D0, the delay and the BARs. Check: no `was asleep` line is the normal case. |
| P10 | The display adapter's BARs are never sized at scan time (P3), but the MTRR fallback needs the size of the one that holds the framebuffer. | `pci_display_bar` | | **fixed**: sized on demand, only when the screen measured slow (F8), with interrupts off and decoding left on (Linux's `mmio_always_on` way). Only BARs that start at or below the framebuffer are touched. |

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
* The queue depths against the controller's limits: the NVMe I/O queue was always `min(MQES+1, nvme.depth)`; the 16-entry admin queue is now
  clamped to `CAP.MQES` as well (**fixed**; every real controller has MQES >= 63, so this changes nothing on a working one). AHCI uses command
  slot 0 only (one command at a time), so there is no depth to clamp.

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

* `e1000.c` (I219 family on the target): the network branch has since added the PCH-specific care (ME handshake, PHY semaphore and power-up,
  explicit ring registers, a transmit watchdog), which is its owner's to review. One thing seen while reading: `pci_enable` did not check or clear the
  PCI power state first (only the xHCI driver did); since the second round it does for every driver (P9). The MAC address comes from RAL/RAH.
  Boot is not affected either way: the driver logs and leaves a card that does not answer alone.
* `audio/hda.c`: matches by class (`04/03`), all waits are bounded (reset 100-200 ms, codec wait up to 500 ms, stream resets 5-20 ms), so a codec that does not
  answer costs less than a second of boot.

## Checklist for the first boot on the real machine

1. Photograph the screen at three moments: right after the loader (first screen), when the log stops scrolling, and with the desktop up.
2. Read: CPU line, `memory map`, `framebuffer ... write-combining`, `framebuffer speed` (should be hundreds of MB/s or more), `acpi: ...`,
   `TSC references`, `pci: N devices`, `xhci ...`, `ahci0 ...`/`nvme0 ...` (and that `blk.ro` is in the `cmdline` shown in the `image` line).
   Once the log has scrolled away (or on the desktop, in a terminal window):
   `dmesg | grep -E "cpu:|memory map|framebuffer|TSC|acpi|APIC|xhci|ahci|nvme|storage|boot complete"`.
3. If the log scrolls slowly: the kernel has already tried to fix it (F8); read the `fb.mtrr=auto` / `mtrr:` / `framebuffer: ... MTRR` lines after the PCI
   lines: they say what the firmware's MTRRs are, which BAR holds the screen, and either `fb speed before N MB/s after M MB/s, kept` or why nothing was
   added. If the speed is still low, boot once with `fb.cache=uc` and compare the two `framebuffer speed` lines (F7): equal speed means the display
   memory itself is the limit, not the mapping.
4. If the keyboard does not work: `[Satori] no USB keyboard is bound 8 s after boot; xhci ... ports (number:PORTSC): ...` is the line to photograph.
5. On the desktop: `cat /tmp/fairyd.stat` in a terminal; compose and flush microseconds per frame, and the flush speed.
6. If the machine resets by itself a few seconds after the picture appears: suspect memory the firmware still used (L5/C4 should have made that
   impossible) or an unhandled interrupt source (C10).
7. If the screen was not photographed in time (or the machine froze): pull the stick and read `TOUHOU.LOG` (this boot) and `TOUHOU.PRV` (the boot
   before) in the root of its big partition on another computer, see [REAL_HARDWARE.md](REAL_HARDWARE.md). They hold the same lines as the screen,
   the audit lines above included, up to about five seconds before the end; a file without the "clean run" line at its end belongs to a boot that did not finish.

8. If the stick is not mounted (no `/HumanVillage`, `TOUHOU.LOG` missing on the stick afterwards): read the `usb-storage ...` lines (vendor:product, size,
   `writable` / `given up on` / `did not become ready`) and the `[Keine]` lines right after them; `usb=debug` on the command line adds every command. The
   fallbacks for sticks that behave badly are still being written (U1-U11 above).

## Tests

`python tests/run_all.py hwaudit` runs the audit tests: PIT missing, other processors parked, a CPU without PAT, `fb.cache=uc`, the MTRR fallback
(`hwaudit_mtrr_selftest`, `hwaudit_mtrr_forced_attempt`, `hwaudit_mtrr_knob_off_and_auto`), `hwaudit_pci_selftest` (BAR sizes, D3hot to D0 with an e1000e),
no PS/2 controller, 6 GiB of RAM, cmdline.txt encodings, missing cmdline.txt, the fbcon self test, the pixel-exact scroll check and fairyd's statistics and
knobs. The `selftest` test includes the fbcon, MTRR and PCI checks. `tools/qemu.py` honours `TOUHOU_CPU=Skylake-Client-noTSX-IBRS` (set by another branch) if you want
the guest to look like the target's CPU family.

## State when paused

The second round (asked for after the first audit was merged) was stopped on request in the middle of its second item. Branch
`worktree-agent-a6d5e6306014eb8c7`; the head builds. Main at the start of the round was the merge of the first audit (`f9358db`) and had not moved.

**Done, built, tested**

* Write-combining MTRR as a measured, automatic fallback with the knob `fb.mtrr=off|on|auto` (F7, F8): `kernel/arch/x86_64/mtrr.c`, `kernel/gfx/fbtune.c`,
  `kernel/include/mtrr.h`, `fbcon_speed_probe` in `fbcon.c`, `fb_set_map_flags` in `fb.c`, `pci_display_bar` in `pci.c`.
* PCI: the BAR sizing bug (P8), the D0 wake-up in `pci_enable` for every driver (P9), the BAR lookup for the display adapter (P10); the NVMe admin queue is
  clamped to `CAP.MQES`; `xhci.c` lost its private D0 code.
* Tests added to `tests/case_hwaudit.py`: `hwaudit_mtrr_selftest`, `hwaudit_mtrr_forced_attempt`, `hwaudit_mtrr_knob_off_and_auto`, `hwaudit_pci_selftest`.
  Run before the pause and all passed: those four, `hwaudit_fbcon_selftest`, `hwaudit_framebuffer_cache_knob`, `blk_nvme_read`, `blk_nvme_errors`,
  `blk_nvme_timeout`, `blk_nvme_mdts`, `image_nvme_and_virtio_boot` (every QEMU boot also goes through the new `pci_enable` for the AHCI controller). One
  early failure of `hwaudit_pci_selftest` was the test's own timing margin (it measured 8 ms for a 10 ms wait on the millisecond clock); the check now
  allows 7.
* **Not re-run after those changes**: the USB tests (xhci.c lost its private D0 block; the wake-up is a no-op for a function in D0, which QEMU's always is), the
  network and audio tests (also through `pci_enable`), the whole `hwaudit` group in one go, the desktop tests. Run `python tests/run_all.py` before merging.

**Half done: USB mass storage soft fallbacks (U1-U11 above)**, parked, because it does not compile yet, on the branch `wip/usb-storage-soft-fallbacks` (one
commit, `5bd592f`, only `kernel/drivers/usb/storage.c`). In it: the header comment with the new knobs, the quirk machinery (empty table,
`usbstorage.quirk=`, parser, flag names), the size ladder helpers, a `csw_check` that no longer judges the residue, `residue_clamped`, the fault
variables and their use in `msc_run` (`maxlen`, `modehang`, `residue`, `badresidue`), a rewritten `msc_io_one` (splits by `Msc.chunk`, trusts the bytes that
moved, ladder on the second failure, a hang means reset and go down at once, `msc_note`), `msc_blk_rw` reading the chunk each round, a `lun_write_protected` that
survives a hang, `msc_run_boot`/`msc_run_boot_retry` (two resets for a bring-up command that hangs), and the 1 s first-command delay.
Still to write before it builds and works: `cfg_load` must parse the new faults (`maxlen`, `modehang`, `lunhang`, `residue`, `badresidue`, `cap10max`,
`rc16illegal`) and keep `usbstorage.quirk=` in `g_quirk_opt`; `msc_probe` must set `Msc.chunk` (`MSC_CHUNK`, or the chunk quirk), `Msc.quirks`
(`quirks_for`), the first-command delay (`slowstart`), log the quirks, treat a GET MAX LUN timeout as one LUN (clear `dev->broken`, fault `lunhang`);
`lun_step` (INQUIRY) and `lun_ready` must call `msc_run_boot`; `lun_capacity` must use `msc_run_boot_retry`, map block size 0 to 512, fall back to 2^32
blocks when READ CAPACITY(10) says 0xFFFFFFFF and READ CAPACITY(16) is refused, honour `fixcap` and the faults `cap10max`/`rc16illegal`, and fill
`MscLun.cap_how`; `Q_NO_SYNC` must set `l->no_sync`; `lun_register` should print `cap_how`, `wp_how`, the chunk and the quirks; `msc_describe` should show chunk,
quirks and the residue counter.

**Not started**: U10 (re-enumerating a stick in place when its bulk-only reset fails), U11's one-line `usb: summary` before "boot complete", a section in
`docs/USB.md` for the new knobs, the tests below.

## How to resume

1. `git checkout worktree-agent-a6d5e6306014eb8c7` (or merge it into main first), `python tools/build.py`, `python tests/run_all.py hwaudit` (15 tests, all
   passed before the pause), then the full suite once, because the regression runs listed above were not repeated.
2. The USB work: the owner of `storage.c` and `mountpolicy.c` is the USB agent, who was editing the FAT dirty-volume checks (`fat*.c`, `mountpolicy.c`) at
   the time; the diff in `storage.c` is large (about 300 lines). Bring it over with `git checkout wip/usb-storage-soft-fallbacks -- kernel/drivers/usb/storage.c`
   (if `storage.c` changed on main meanwhile: `git merge wip/usb-storage-soft-fallbacks` and resolve), finish the list above, build, then fix `selftest()`
   in `storage.c`: a residue above the request is now accepted and clamped (`EXPECT(!csw_check(...))`), and add checks for `quirks_for`/`quirk_entry`,
   `ladder_below` and `residue_clamped`. Agree the scope with the USB agent first if a smaller patch is preferred (the ladder and the residue rule matter
   most; the quirk table can follow).
3. Tests to write in `tests/case_usbstorage.py` (each needs its fault in `cfg_load`):

   | Test | Command line | Must show |
   |------|--------------|-----------|
   | ladder | `usbstorage.fault=maxlen:16384 usbstorage.timeout=1500` | `hung the stick ... at most 32 KiB`, then `at most 16 KiB`; 35 MiB read back identical, a 2.7 MB file written and read back by the host; no `given up on` |
   | residue | `usbstorage.fault=residue:50,badresidue:60` | data identical, `0 reset recoveries`, the residue counter above 0 |
   | MODE SENSE hang | `usbstorage.fault=modehang:1 usbstorage.timeout=2000` | `MODE SENSE hung the stick`, the disk online and writable, one reset recovery |
   | GET MAX LUN hang | `usbstorage.fault=lunhang:1` | online with one LUN |
   | capacity | `usbstorage.fault=cap10max:1,rc16illegal:1` | 4294967296 blocks of 512, no `ignored` |
   | bring-up nudge | `usbstorage.fault=drop:2 usbstorage.timeout=1500` | `got no answer; the stick was reset (1 of 2 tries)` and the stick online. The existing `usbstor_silent_stick_bounded` expects exactly this fault to condemn the stick, so it needs a persistent fault (for example a new `deaf:2`: every command from the second on) to keep testing "a stick that stays silent is given up on" |
   | quirks | `usbstorage.quirk=46f4:0001:chunk4k,fixcap` | `/proc/usbstorage` shows 204799 blocks and a 4 KiB chunk, reads and writes correct |

   Then `python tests/run_all.py usbstor,hwaudit,selftest`.
4. The remaining candidates after that: U10 and U11, MTRR copying for a future SMP start (F8), ECAM/MCFG (P2, not needed), and the checks that only the real
   machine can do (the checklist for the first boot, above).
