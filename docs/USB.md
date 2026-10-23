# USB

USB host support exists so that a keyboard and a mouse work on machines that have no PS/2 port (the target is an Intel
Kaby Lake desktop whose only keyboard controller is the PCH xHCI, `8086:a12f`). It is written for real silicon first and
verified against QEMU's two xHCI models (`qemu-xhci`, `nec-usb-xhci`) with 29 scripted tests in `tests/case_usb.py`.

Scope: xHCI host controller, enumeration, hubs (USB 2 with transaction translators, USB 3), HID boot keyboards and mice
(plus report-descriptor driven mice and keyboards), bulk transfers, and the **bulk-only mass storage class driver**
(sticks, card readers and enclosures become `sda`, `sdb`, ... in the block layer; see "Mass storage" below). Not covered:
EHCI/OHCI/UHCI (the old companion controllers), isochronous transfers, UAS (USB Attached SCSI) and the other mass storage
protocols (CBI, ATAPI-style subclasses), audio and any class driver beyond hub, HID and mass storage, USB 3 power management
(U1/U2/LPM), suspend/resume, remote wakeup.

## Layers

```
kernel/include/usb.h            shared types: UsbDevice, UsbHub, UsbHc ops table, class driver interface, services
kernel/drivers/usb/xhci.h       xHCI register / TRB / context definitions (private)
kernel/drivers/usb/xhci.c       host controller driver (the only UsbHc implementation)
kernel/drivers/usb/usb.c        core: usbd thread, port events, enumeration, descriptors, device tree, /proc/usb
kernel/drivers/usb/hub.c        hub class driver (USB 2 and USB 3 hubs)
kernel/drivers/usb/hid.c        HID class driver: keyboards and mice -> input_key() / input_mouse()
kernel/drivers/usb/storage.c    mass storage class driver (bulk-only transport + SCSI) -> block layer, /proc/usbstorage
tests/case_usb.py               the tests of the bus, hubs, HID
tests/case_usbstorage.py        the tests of the mass storage driver and of HUMANVILLAGE on a stick
```

`usb_init()` (strong version of the weak stub in `driver_stubs.c`, called by `kmain` after PCI/storage init) only looks for
class `0C/03/30` devices, claims them (`/proc/pci` shows `[xhci]`), registers `/proc/usb` and `/proc/usbstat`, and creates
the `usbd` thread. Everything else, including the controller reset, happens in that thread, so boot is never delayed.

### One thread, no interrupts, no locks

`usbd` loops at about 1 kHz: drain the event ring, run deferred endpoint recovery, service hubs that reported port changes,
write keyboard LEDs, sleep one tick. Interrupters stay masked (`USBCMD.INTE = 0`, `IMAN.IE = 0`, PCI INTx disabled); the
controller still writes events to the ring, which is what the thread reads. All state is owned by that thread, so there are no
locks and no interrupt routing to get wrong. `/proc/usb` is a text snapshot rebuilt after every topology change and copied out
under `irq_save`.

Blocking operations (commands, control transfers, port resets) are written as straight-line code in the thread. While they wait
they keep draining the event ring (`wait_flag`, `usb_msleep`), so keystrokes from devices that are already working are not
delayed by an enumeration in progress. Callbacks (HID report handlers) run inside that draining and therefore never start control
transfers; the only deferred work is keyboard LED writes (`hid_service`) and endpoint error recovery (`xhci_service`).

Other threads never call into the stack directly: `usb_call(fn, arg)` runs `fn(arg)` in the usbd thread between two polling rounds
and the caller sleeps until it is done (see "Writing another class driver" below). `usb_call` also wakes usbd at once, so its idle
1 ms sleep does not add a tick of latency to every block request.

**Boot rendezvous.** `kmain` calls `usb_wait_settled(8000)` right before it starts the first session. It returns at once when there
is no xHCI controller (or `usb=off`, or the controller failed), and when nothing was plugged in when the controller came up. Otherwise it
waits until the bus is quiet: every controller started, no port left to examine, no enumeration in progress, no mass storage unit still
getting ready, the automount of a stick's HUMANVILLAGE volume finished, and nothing new for 150 ms (hub ports report late). The wait
is bounded at 8 s whatever happens (`[Kappa] usb: still busy after 8000 ms ...` in the log); a stick that needs longer is mounted by the
hot-plug path when it is ready, after the session has started. A normal boot with a keyboard, a mouse and a stick pays 0.4-1 s.

Every wait has a timeout: commands 5 s (Linux's value), control transfers 2.5-3 s, port resets 1.5 s (warm 4 s), BIOS hand-off 1 s,
HCRST 2 s. If the controller itself stops answering (all-ones register reads, `HSE`/`HCE`, unexpected halt, command ring that cannot be
stopped) `x_die()` prints one line, clears `RUN`, marks the bus dead, releases all keys, and drops every device without freeing memory the
controller might still write to.

## xHCI bring-up (xhci.c, `xhci_start`)

1. `xhci_create` (at `usb_init` time): PCI power state to D0 if the BIOS left it asleep; memory space and bus master on, INTx off.
2. Map BAR 0 (uncached). Sanity-check CAPLENGTH, the doorbell and runtime offsets against the BAR size; read HCSPARAMS/HCCPARAMS.
   Context size comes from `HCCPARAMS1.CSZ` (32 or 64 bytes, both implemented); 64-bit addressing from `AC64`.
3. Walk the extended capabilities: Supported Protocol (which root ports are USB 2 / USB 3, protocol slot type, speed-ID tables
   including PSI entries) and USB Legacy Support.
4. **BIOS hand-off** (Linux `quirk_usb_handoff_xhci`): if `HC BIOS Owned` is set, set `HC OS Owned`, wait up to 1 s for the BIOS bit to
   clear (take the controller anyway on timeout, with a log line), then clear every SMI enable in `USBLEGCTLSTS` and acknowledge the
   RW1C SMI status bits. Without this the firmware keeps servicing the controller from SMM.
5. Intel chipsets that have an EHCI to share ports with (7/8/9 series): write the port routing registers so all ports belong to xHCI.
   Skipped on Sunrise Point and later.
6. Wait for `CNR` = 0, halt (`RUN` = 0, wait `HCH` 200 ms), `HCRST`, 2 ms pause on Intel hosts, wait for `HCRST` to clear, wait for `CNR`.
   Controllers that do not halt or reset are left alone with a log line.
7. Allocate with `dma_alloc` (zeroed, page aligned, checked against the DMA range and RAM size): DCBAA, scratchpad array and
   `MaxScratchpadBufs` 4 KiB pages (from the split HCSPARAMS2 fields), command ring, one event ring + ERST entry, control bounce page.
8. `CONFIG.MaxSlotsEn`, `DCBAAP`, `CRCR` (ring base | RCS), then `ERSTSZ`, `ERDP`, `ERSTBA` in that order; clear `USBSTS` RW1C bits;
   `USBCMD.RUN`; wait for `HCH` to clear. All 64-bit registers are written low dword first, then high (Linux `lo_hi_writeq`).
9. Issue a No-Op command: a controller whose command ring does not answer is not used.
10. Power every port that is not powered, wait 150 ms, wait 100 ms more for USB 2 debounce and USB 3 link training, then look at every
    port once, clear stale change bits, and queue ports with something attached. After that Port Status Change events drive everything.
    Two seconds later one more look at all ports catches a connect whose event was lost and USB 3 links stuck in Polling or
    compliance mode (those get a warm reset, as Linux does for ports without Cold Attach Status).

### Rings and contexts

* All rings are 256 TRBs (one page); the last TRB is a Link TRB with Toggle Cycle. The producer writes parameter and status first, a
  compiler barrier, then the control dword with the cycle bit, so the controller never sees a half-written TRB. When the enqueue index
  reaches the link TRB, the link gets the current cycle bit and the producer cycle toggles. Doorbells are preceded by `mfence`.
* Event ring consumption: valid when the cycle bit equals the consumer cycle state; after a batch, `ERDP` is written with the new
  dequeue pointer and `EHB` set (RW1C), as Linux does.
* Input contexts are built from scratch for each command, slot context copied from the output context where Linux does so. The
  default control endpoint context is re-pointed at the ring's *current* enqueue position before the second Address Device.
* Interval conversion follows `xhci_get_endpoint_interval()`: exponent (bInterval - 1) for high/super speed, frames converted to
  microframes and rounded down to a power of two (clamped 3..10) for low/full speed.

### PORTSC

Writes never touch RW1C bits by accident: a write starts from `v & PORTSC_PRESERVE` (read-only bits, PLS, PP, PIC, wake enables)
and ORs in only the bits meant (PR, WPR, or the specific change bits to acknowledge). `PED` is RW1C ("1 disables the port"), so
it is never written back. USB 2 resets use `PR`; USB 3 ports use `PR` (hot reset) normally and `WPR` (warm reset) when the link is
`Inactive`, in compliance mode, or `CAS` (cold attach status) is set, limited to three warm resets per port until the link comes up.

## Enumeration (usb.c)

Port change (event or initial scan) -> `port_event`: read PORTSC (or hub class status), acknowledge change bits, disconnect the
old device if the connection changed or the port was disabled, then for a connected port with no device `port_connect`:

1. Debounce (100 ms stable, 2 s limit, Linux's HUB_DEBOUNCE_STABLE / TIMEOUT; skipped for USB 3 links that need a warm reset).
2. Port reset; up to four attempts, each starts with a fresh reset. Speed comes from PORTSC (root) or the hub's port status.
3. Enable Slot (protocol slot type), allocate output/input contexts and the default control ring, `DCBAA[slot]`.
4. Attempts 1-2 use the **new scheme**: Address Device with BSR (slot goes to Default, no SET_ADDRESS yet), read the first 8 bytes
   of the device descriptor, fix the control endpoint size with Evaluate Context if the guess (8 low, 64 full/high, 512 super) was
   wrong, Address Device again (the controller sends SET_ADDRESS), wait 10 ms. Attempts 3-4 use the **old scheme** (SET_ADDRESS
   first). Descriptor reads retry three times 50 ms apart.
5. Read the full device descriptor and the configuration descriptor (length from the 9 byte header, capped at 4096). Parse interfaces
   (alternate setting 0), endpoints, SuperSpeed endpoint companions, class descriptors that precede the first endpoint (HID).
6. Bind drivers (`match`), Configure Endpoint for the interrupt-IN endpoints of the bound interfaces (before SET_CONFIGURATION, like
   Linux), SET_CONFIGURATION, `probe`. Devices nothing binds to stay unconfigured (100 mA) and are only listed.
7. Read manufacturer / product / serial strings (language id from string 0), log `usb 1-3.2: new high-speed device vvvv:pppp "..."`.

Route string: the port number (capped at 15) goes into nibble `depth` of the parent's route. Root Hub Port Number is the top of the
chain. Low/full-speed devices below a high-speed hub get **TT Hub Slot ID and TT Port Number** in the slot context (a hub below
that hub inherits the same translator), plus the MTT bit if the hub has one translator per port. At most five hubs deep.

Disconnect (`usb_disconnect`): mark dead, driver `disconnect` (keyboards release held keys, mice release buttons, hubs disconnect
their children first), Disable Slot, free contexts and rings, remove from the tree, rebuild `/proc/usb`.

### Error recovery (xhci.c)

* A stalled or failed control transfer halts the control endpoint: Reset Endpoint, then Set TR Dequeue Pointer to the current enqueue
  position, so the next transfer starts clean (`usb=stalltest` provokes this on every device).
* A control transfer or bulk transfer that times out additionally gets Stop Endpoint first; the device is then marked `broken`.
* A bulk transfer to a device on a **root port** also ends the moment the port stops reporting a connection (`-ENODEV`, device marked
  `broken`): QEMU drops the transfers of an unplugged device without completing them, and a pulled stick should not hold up every
  request for the full timeout. Devices behind a hub still wait for the timeout (asking the hub needs the control pipe).
* A command that times out aborts the command ring (`CRCR.CA`), waits for `CRR` to clear, moves the dequeue pointer past the stuck
  command and carries on. If the ring does not stop, the controller is declared dead.
* Interrupt endpoints that report errors are recovered from the thread (never from inside event processing): up to four tries,
  then polling of that endpoint stops with a log line.
* A device whose setup fails is disabled (Disable Slot), its port is reset again and the whole sequence restarts, four times, then
  the port is ignored until the connection changes.
* `usbfault=ctl:N` / `usbfault=cmd:N` (kernel command line) drop the doorbell of the Nth control transfer / command so these paths
  can be tested in QEMU; `tests/case_usb.py` does.

## Hubs (hub.c)

* Hub descriptor (type 0x29, or 0x2A for SuperSpeed). High-speed hubs whose *device* descriptor says protocol 2 (one translator per
  port) get alternate setting 1 selected. SuperSpeed hubs get SET_HUB_DEPTH.
* The controller learns about the hub through the slot context (Hub, MTT, Number of Ports, TT think time) with the command Linux's
  `xhci_update_hub_device()` uses: Configure Endpoint for xHCI 1.0 and later, Evaluate Context for 0.95. The output context is read
  back and the other command tried if the Hub bit did not stick (QEMU ignores both). SET_HUB_DEPTH follows, then port power.
* All ports are powered (SET_FEATURE PORT_POWER), then 2 x bPwrOn2PwrGood (at least 100 ms) is waited out. The status change
  endpoint is polled by the controller driver; it marks ports (bit n = port n) and hub-level changes (bit 0, acknowledged).
* Port status is converted from the USB 2 or USB 3 layout (power at bit 8 vs 9, speed bits, link state, BH reset) into
  `UsbPortStatus`; reset is `PORT_RESET` or `BH_PORT_RESET` followed by polling `C_PORT_RESET`, then 50 ms recovery.

## HID (hid.c)

* **Boot keyboards** (class 3, subclass 1, protocol 1): SET_PROTOCOL(boot) (if that is refused and the report descriptor decodes as
  a keyboard, the device's own format is used instead), SET_IDLE(0), 8 byte reports. Pressed usages are kept as a 256 bit set per
  keyboard; each report is diffed against it: releases first, then presses with modifiers before ordinary keys, so Shift+A in one
  report arrives as Shift, A. Reports whose six key slots only contain ErrorRollOver / POSTFail are ignored. Usage page 7 maps to
  every `KEY_*` code including keypad, F1-F12, navigation, GUI keys and Menu. PrintScreen and Pause have no code in `keys.h` and are
  dropped.
* A reference count per key code lets several keyboards (or several interfaces of one receiver) share keys without double
  presses; the input layer sees one press and one release. Key repeat is the input layer's (`input_tick`), nothing is generated
  here. `input_key` / `input_mouse` are called with interrupts off because the PS/2 handlers and the timer tick share that state.
* **Mice**: the report descriptor is fetched and parsed (items, push/pop, usage ranges, report IDs, 1-32 bit fields at any bit
  offset). If it describes relative X and Y (and optionally wheel and buttons 1-3) the mouse stays in report protocol and the fields
  are read where the descriptor puts them (12 or 16 bit deltas on Logitech receivers, for example); otherwise SET_PROTOCOL(boot)
  and the 3/4 byte boot report. Buttons are OR-ed across all mice. Absolute pointers (tablets) are declined.
* **Interfaces that are not boot class** are examined only if the device has no boot HID interface at all, and are bound if their
  report descriptor holds a relative mouse or a keyboard (modifier bits + key array; bitmap-only NKRO descriptors are left alone,
  those keyboards also expose a boot interface).
* **LEDs**: Num Lock starts on (matching the input layer), Caps Lock follows `keyboard_modifiers()`, Scroll Lock toggles. The SET_REPORT
  is sent from the thread's housekeeping step, not from the report callback; a keyboard that stalls it is not asked again.

## Mass storage (storage.c)

A stick, a card reader or a USB enclosure that speaks SCSI over the **bulk-only transport** (interface class 8, subclass 6, protocol
0x50) becomes a disk in the block layer: `sdb`, `sdc`, ... (`driver "usb-storage"` in `/proc/partitions` and `lsblk`, model
`VENDOR PRODUCT` from INQUIRY), with `sdb1`, `sdb2` for the partitions the GPT/MBR scan finds. Names come from the pool the SATA driver
uses (`blk_register_sd`: the first free letter), so a stick plugged in after boot never collides with a disk and a pulled stick gives its
letter back. UAS (protocol 0x62), CBI and the ATAPI-style subclasses are ignored with one log line (a UAS-capable stick also offers the
bulk-only interface as alternate setting 0, which is what gets driven). `usbstorage=off` leaves all of it alone.

```
[Kappa] usb-storage 1-1: 46f4:0001, bulk-only transport, 1 LUN -- the kappa are sizing the stick up
[Marisa] blk: sdb  usb-storage  100 MiB, 512-byte sectors  QEMU QEMU HARDDISK
[Marisa] blk: sdb1  34 MiB  start 2048  label TOUHOUOS-ESP
[Marisa] blk: sdb2  64 MiB  start 71680  label HUMANVILLAGE
[Kappa] usb-storage 1-1 lun 0: "QEMU QEMU HARDDISK", 100 MiB in 512-byte blocks, writable -- carried over the river as sdb
[Keine] fat32: sdb2 mounted on /HumanVillage, 64 MiB, 1024-byte clusters, label 'HUMANVILLAG', home directories per user
[Keine] the Human Village keeps its records on the stick now (sdb2, found by partition label): the homes persist from here on
```

**Bring-up is a state machine, not a blocking probe.** `probe` only asks GET MAX LUN and records the LUNs; then, one SCSI command per
usbd round and LUN (so hubs and keyboards keep being served while a stick spins up), each LUN goes through
`START` (INQUIRY: type 0x00/0x0E/0x07 is a disk, a qualifier or type 0x1F means "no device at this LUN", anything else is logged and
ignored) -> `WAIT` (TEST UNIT READY; on CHECK CONDITION REQUEST SENSE decides: "becoming ready" and unit attention are tried again every
250 ms for at most 20 s, "medium not present" goes to `NOMEDIA`, a unit that does not implement the command is taken as ready) ->
READ CAPACITY(10) (and (16) when the last LBA is 0xFFFFFFFF) -> MODE SENSE(6) for the write-protect bit -> `ONLINE`
(`blk_register_sd`, then the mount policy hears about it, see STORAGE.md). `NOMEDIA` (a card reader without a card) asks again every two
seconds. `/proc/usbstorage` shows each LUN's state:

```
usb-storage 1-1: 46f4:0001 bulk-only, 1 LUN, 0 reset recoveries
  lun 0: online "QEMU QEMU HARDDISK" sdb 204800 x 512 rw  reads 9 writes 10 syncs 17 errors 0
```

**I/O.** `BlockDev.read/write/flush` run on arbitrary threads and go through `usb_call`; each hand-over is one SCSI command of at most
64 KiB (one bulk transfer), so a 4 MiB filesystem request is 64 hand-overs and usbd serves its other work in between. READ/WRITE(10) is
used until a request needs 64-bit LBAs or 32-bit counts (or the unit's capacity needed READ CAPACITY(16); `usbstorage.rw16` forces the
16-byte forms for testing). **Write policy** is the one of the SATA and NVMe drivers: every write request is followed by one
SYNCHRONIZE CACHE(10); a stick that answers ILLEGAL REQUEST (or fails the transfer twice) is not asked again, and the write is not failed
for it. `blk.ro` does *not* apply to sticks (it is for the internal disks); a stick follows its own write-protect switch, and a
DATA PROTECT answer to a write switches the disk to read-only on the spot.

**Everything the device says is checked.** CSW: signature, tag, status byte (0 or 1; 2 is a phase error), residue not larger than the
request, and for a "good" read that exactly the requested bytes arrived. Sense data: only the fixed (70h/71h) and descriptor (72h/73h)
formats are believed, anything else is a plain failure. Capacity: block size zero, not a power of two, or other than 512/4096 ->
the LUN is ignored with the reason in the log; `last LBA + 1` overflowing or more than a pebibyte -> ignored. GET MAX LUN above 15 is taken
to mean one LUN, more than 8 LUNs use the first 8. A wrapper the stick will not take, a stalled data or status stage, a bad CSW: **bulk-only
reset recovery** (class request 0xFF, then CLEAR_FEATURE(HALT) on bulk-in and bulk-out) and one more try, up to three per request; if the
recovery itself fails, or a transfer times out (5 s for commands, 15 s for reads, 30 s for writes and SYNCHRONIZE CACHE), the stick is
"given up on": its disks are unregistered, every later request returns `-ENODEV`, nothing else is sent to it. `usbstorage.selftest` runs
the validators against hand-made hostile answers (29 checks).

**Removable media.** A request that finds "medium not present" drops the disk (`-ENODEV` to the caller, the partitions go with it) and the
unit is polled until a medium returns; "unit attention: medium may have changed" (06/28) drops the disk too and probes the unit again from
the start, so data is never written to a different medium than the one a filesystem was mounted on. A mounted volume on a dropped disk
keeps failing with `-ENODEV` until it is unmounted.

**Hot-unplug.** `disconnect` unregisters the disks (`blk_unregister`: the `/dev` nodes, `/proc/partitions` entries and partitions go; the
`BlockDev` structs stay allocated because nothing counts who still holds a pointer: a few hundred bytes per unplugged stick) and marks the
device gone, so requests that were queued behind the disconnect fail with `-ENODEV` instead of touching freed memory. A transfer that is in
flight on a root port ends as soon as the port reports the disconnect; behind a hub it ends at its timeout. The FAT volume mounted from a
pulled stick logs one line (`fat32: sdb2 vanished while it was mounted ...`), keeps returning errors, and `umount` drops it without a
write-back (the volume stays flagged dirty).

Nothing in the driver writes on its own: WRITE(10/16) and SYNCHRONIZE CACHE exist only on the path of `BlockDev.write` / `flush`, i.e.
for a filesystem the policy or `mount` mounted, or for root writing the `/dev` node. The partition scan only reads.

## Files in /proc

`/proc/usbstorage` is described above. `/proc/usb` (what `lsusb` prints), one line per device, interface lines indented (an example, not this machine's real port counts):

```
Bus 1: xHCI 8086:a12f at 00:14.0, v1.00, 26 ports (16 usb2, 10 usb3), 32 slots, 32-byte contexts
Bus 1 Port 3: ID 046d:c52b Logitech USB Receiver, high-speed, class 00, driver hid-keyboard,hid-mouse
    If 0: class 03/01/01, 1 endpoint, driver hid-keyboard
    If 1: class 03/01/02, 1 endpoint, driver hid-mouse
    If 2: class 03/00/00, 1 endpoint, driver none
Bus 1 Port 7.2: ID 0627:0001 QEMU QEMU USB Keyboard, full-speed, class 00, driver hid-keyboard
```

`Port 7.2` is hub port 2 of the hub on root port 7. A controller that failed or died is listed with `-- failed:` / `-- dead:` and the reason.

`/proc/usbstat` (test and debug aid): `keyboards`, `mice`, `kbd_reports`, `key_down`, `key_up`, `keys_held`, `mouse_reports`,
`mouse_dx`, `mouse_dy`, `mouse_wheel` (sums of what the drivers fed to the input layer), `mouse_x`, `mouse_y`, `mouse_buttons`
(the input layer's own state), `leds`. There is no `/dev/input/events` node yet, so the tests read this instead.

## Kernel command line

| Option | Effect |
|--------|--------|
| `usb=off` | do not touch any USB controller at all (no `/proc/usb`; BIOS keyboard emulation, if any, keeps working) |
| `usb=debug` | verbose log lines with timestamps (port events, enumeration steps, command failures) |
| `usb=hidreport` | debug + parse report descriptors even for boot interfaces (exercises the descriptor-driven paths) |
| `usb=stalltest` | debug + send a request every device must STALL and verify the control pipe recovers |
| `usb=bulktest` | SCSI INQUIRY / READ CAPACITY / 64 KiB READ(10) on any bulk-only mass storage device (read only, no disk is registered; the mass storage driver stays unbound while it is set) |
| `usbstorage=off` | do not bind mass storage devices (sticks are listed by `lsusb` with `driver none`) |
| `usbstorage.nosync` | no SYNCHRONIZE CACHE after write requests |
| `usbstorage.rw16` | READ CAPACITY(16) and READ/WRITE(16) even for small devices |
| `usbstorage.selftest` | run the validators against hand-made hostile device answers; one log line with the verdict |
| `usbstorage.timeout=MS` | every bulk wait at most MS ms (tests; a timeout gives the stick up) |
| `usbstorage.fault=csw:N,stall:N,drop:N,notready:MS,maxlun:N` | fault injection for the tests, several separated by commas: the N-th status wrapper is corrupted / the N-th command wrapper stalls the pipe / the N-th command gets no answer / every unit is "not ready" for MS ms / GET MAX LUN answers N |
| `usb=parsertest` | run the HID report descriptor parser and decoders against built-in descriptors at boot (one log line with the result) |
| `usbfault=ctl:N`, `usbfault=cmd:N` | never ring the doorbell for the Nth control transfer / command (counted from boot; the start-up No-Op is command 1) |
| `usbring=N` | use only N (16..256) entries of every ring, so cycle-bit / link TRB wrap-around happens after N-1 TRBs; `USB_RING=16 python tests/run_all.py usb_` runs the whole USB suite that way |

## Real hardware: what was handled and what to know

Intel Series 100/200 PCH xHCI (`8086:a12f`, `a2af`), from the Linux quirk list and the datasheet behaviour:

* **Firmware ownership** (above): BIOS-owned semaphore, SMI enables, 1 s timeout. The controller is reset right after, so whatever
  the firmware had configured (event ring in memory that is now ours, running endpoints) stops being touched.
* **Reset timing**: 2 ms pause before polling `HCRST` (Intel hosts), then `CNR`. Operational registers are not touched before `CNR` = 0.
* **32-byte contexts**, 64-bit addressing, a handful of scratchpad pages (required: the controller will not run without them).
* **No port routing registers** on this PCH (they exist only where an EHCI shares the ports); the code that writes them is limited to
  7/8/9 series device IDs.
* **PORTSC discipline** (see above): this PCH latches change bits and will not raise another Port Status Change event for a port until
  they are cleared; clearing must not disable the port.
* **USB 3 ports**: link training takes up to a few hundred ms after reset or connect, so connects arrive as events; a link stuck in
  `Inactive`/compliance/`CAS` gets warm resets; the speed ID comes from PORTSC and is mapped through the protocol capability's
  speed table (5 Gb/s is normally speed ID 4).
* **Hubs**: many Dell desktops route front-panel ports or a monitor's ports through a high-speed hub; low/full-speed keyboards behind
  one need the TT fields in the slot context, which are set. Hubs with one translator per port (device protocol 2) are switched to it.
* **Not enabled**: LPM, U1/U2 exit latencies, interrupt moderation, MSI/MSI-X, Event Data TRBs, BEI. Linux enables or works around some
  of these for suspend, power and throughput; none is needed for a polled keyboard and mouse.
* **Boot-time window**: UEFI firmware may leave the xHCI running with its rings in memory that the loader already treats as free RAM
  (EfiBootServicesData). The controller is reset by the first thing `usbd` does, a fraction of a second after PCI init; before that, a
  keyboard event could in principle make the firmware's controller write into a page the kernel has reused. Firmware drivers normally halt
  the controller at ExitBootServices, so this has not been seen, but it is a window a loader-side reset would close.
* **IOMMU**: the code assumes DMA addresses are physical addresses. A firmware that leaves VT-d DMA remapping switched on with blocking
  tables (Kernel DMA Protection) would make every transfer fail with host system errors; the log would say so.

### If the keyboard does not work on the real PC

Boot with a serial console or photograph the screen; the USB lines are tagged `Kappa` (hardware), `Satori` (keyboard) and `Koishi`
(mouse). From the top:

1. `usb: disabled on the kernel command line (usb=off)`: remove `usb=off` from `cmdline.txt` on the boot volume.
2. `usb: 00:14.0 is a EHCI controller; only xHCI is supported`, or no xHCI line at all: the machine's ports hang off an older
   controller. This code cannot drive them. (The PCH `a12f` is xHCI only.)
3. `xhci 00:14.0: start-up failed: ...`. The text says which step: `BAR 0 is missing or unassigned` (firmware did not assign the BAR),
   `registers read as 0xffffffff` (device off the bus / D3 / wrong BAR), `controller not ready after 3 s`, `controller does not halt`,
   `host controller reset did not finish`, `command ring does not answer`. These point at the hand-off or reset sequence.
4. `xhci 00:14.0: firmware did not release the controller within 1 s; taking it anyway`: the SMM handler ignored the request. If the
   keyboard then still fails, look for a BIOS option about xHCI hand-off or legacy USB support. If none helps, boot with `usb=off` to
   keep whatever keyboard emulation the BIOS provides (it is not reachable otherwise once SMIs are off).
5. `xHCI 8086:a12f at 00:14.0, v1.00, ...` followed by `N of M ports have something attached: ...`. If N is 0 but a keyboard is plugged in,
   try another port (a USB 2 port, not through a dock) and look at the `no USB keyboard is bound 8 s after boot; xhci ... ports
   (number:PORTSC): 1:2a0 2:2a0 3:1203 ...` line printed 8 seconds into boot: `PORTSC` bit 0 is connected, bit 1 enabled, bits 5-8
   link state, bits 10-13 speed, bit 9 power, bit 24 cold attach. `2a0` is a powered port with nothing on it.
6. `usb 1-3: setup failed at '...' (-110), attempt 1 of 4`: the step names where the device stopped answering (`address device (BSR)`,
   `device descriptor (8 bytes)`, `device descriptor`, `configuration descriptor`, `set configuration`). `-110` is a timeout, `-5` a
   controller error, `-32` a stall. Then `giving up on the device on this port after 4 attempts`.
7. `usb 1-3: new high-speed device 046d:c52b "..."` but no `keyboard online` line: the device has no boot keyboard interface that this
   driver accepts (see `/proc/usb`, the `If` lines show class/subclass/protocol and which driver took them).
8. `keyboard online` but typing does nothing: `cat /proc/usbstat` (from a serial console or over ssh-like access) shows
   `kbd_reports`, `key_down`; zero reports means the endpoint is not being polled (look for `endpoint 81 keeps failing`).
9. A hub in the path: `usb 1-3: high-speed hub with 4 ports` and then lines for devices `1-3.1` etc. A keyboard behind a hub that
   never shows up points at hub port power or reset (`usb=debug` logs every port event).
10. `usb=debug` on the command line adds timestamped enumeration steps and the first few controller registers.

## Tests

```
python tools/build.py
python tests/run_all.py usb        # the 29 USB tests, about 5 minutes (QEMU's guest clock slows down when the host is busy; the tests wait for events, not for time)
USB_RING=16 python tests/run_all.py usb_   # the same on 15 entry rings
python tests/run_all.py            # everything
```

Covered: enumeration listing / `lsusb` / `lspci`; typing, Shift, Ctrl chords, arrows, Delete, Home/End, keypad, history; every key class
mapped; key repeat; mouse motion, buttons, wheel; each of those again behind a hub; nested hubs and route strings; hub port power;
keyboard unplug/replug (held key released, repeat stops), mouse unplug/replug (held button released), hub unplug/replug; two keyboards;
full-speed devices (control endpoint size correction); a SuperSpeed device; bulk transfers (SuperSpeed and full-speed); a tablet that is
declined; report-descriptor mode; STALL recovery; control transfer and command timeouts; the NEC xHCI model; an xHCI with nothing
attached; no xHCI at all (PS/2 only); `usb=off`.

The mass storage tests (`python tests/run_all.py usbstor`, 25 tests, about 25 minutes) boot QEMU with `-device qemu-xhci` and
`-device usb-storage` sticks built by `tools/mkdisk.py` (they use `-blockdev` nodes, because QEMU deletes an `-drive if=none` backend
when its device is unplugged) and cover: model, size, partitions and the `lsblk` / `/proc/partitions` / `/proc/usbstorage` / `lsusb` rows;
whole-device, partition and unaligned reads against the host's checksums; HUMANVILLAGE mounted before the first session; persistence
across two boots and the image fsck after each poweroff; a write-protected stick; hot-plug after boot (mounted by the hook), unplug while
idle and during a long read, plugging it in again, the dirty flag after a pull; four hostile sticks (blank, garbage GPT, wild GPT, lying
MBR) that must stay untouched; a UAS device that is ignored; two sticks with the same label (the first wins); a stick without the label;
an internal disk that wins over a stick; 4096-byte blocks; FAT time stamps with `tz=`, without, and through `mount`; stalls and
corrupted status wrappers during a read, a write and the bring-up; a stick that never answers; a stick that needs 12 s (and one that
needs 30 s); card-reader behaviour (medium removed, inserted, swapped); empty LUN slots; `usbstorage=off`; the validators' self test; the
kernel log on the stick (clean run, rotation, killed machine, read-only / dirty / `bootlog=off`); and the `mkimage` stick booted through
OVMF twice.

Harness notes: QMP `input-send-event` with a `device` argument names a *display*, and QEMU 11 aborts on any other id, so
`Qemu._input` no longer forwards it. Default routing already works: a usb-kbd becomes the default keyboard when it is created or
plugged, and the guest's polling activates a usb-mouse (`Qemu.select_mouse("HID")` forces it through the monitor's `mouse_set`).
A guest USB keyboard that nothing polls swallows injected keys, which is why the `usb=off` test attaches only a mouse.

## Writing another class driver (the recipe that `storage.c` follows)

A class driver is a `UsbDriver` (`usb.h`): `match`, `probe`, `disconnect`, `label`. Add it to `g_drivers[]` at the top of `usb.c`
(the only edit outside the driver's own file; a periodic hook like `msc_service()` is one more line in the usbd loop). For mass storage:

1. `match(dev, intf)`: interface class 8, subclass 6 (SCSI), protocol 0x50 (bulk-only transport), one bulk-IN and one bulk-OUT
   endpoint in `intf->ep[]`. Look at descriptors only; no traffic. If `match` says yes the core configures **all bulk and interrupt-IN
   endpoints of that interface** with Configure Endpoint before SET_CONFIGURATION (`endpoint->hc` then holds the controller state).
2. `probe(dev, intf)`: runs in the usbd thread after SET_CONFIGURATION. Control requests: `usb_control(dev, 0xA1, 0xFE, 0, intf->number,
   buf, 1, ms)` for GET MAX LUN, `usb_control(dev, 0x21, 0xFF, 0, intf->number, NULL, 0, ms)` for the bulk-only reset. Bulk I/O:
   `usb_bulk(dev, ep, buf, len, timeout_ms)` moves up to 64 KiB per call and returns the byte count or -errno; `-EPIPE` is a stall:
   call `usb_clear_halt(dev, ep)` (the controller side was already reset) and read the CSW. `msc_command()` in `usb.c` (the
   `usb=bulktest` self test) is a complete CBW / data / CSW exchange to copy from.
3. Register the disk: allocate a `BlockDev` (`blk.h`), set `read`/`write`/`flush`, `blk_register_sd()` (SCSI-style names) or
   `blk_register()`. Do slow device bring-up (waiting for "ready") from a service hook, not inside `probe`, or the next device on the
   bus waits for it.
4. **Thread rule.** The block layer calls `read`/`write` from arbitrary tasks, while the whole USB stack belongs to the usbd thread and
   has no locks. Wrap each request in a function and run it with `usb_call(fn, arg)`: it executes in the usbd thread between two polling
   rounds and the caller sleeps until it returns (a request can therefore wait behind an enumeration for a few seconds in the worst
   case). Inside `probe`, `disconnect` and interrupt callbacks you are already in the usbd thread and may call the API directly (callbacks
   must not start control or bulk transfers).
5. `disconnect(dev, intf)`: runs when the device is unplugged; stop accepting requests (fail them with -ENODEV) before returning,
   because the endpoints and the slot are freed right after. Take the disks away with `blk_unregister()` (requests that are already
   queued behind the disconnect must find a flag that says "gone", not a freed `UsbDevice`: keep the driver state allocated while a
   `BlockDev` points at it) and never take a lock that a thread waiting for a `usb_call` may hold (a filesystem's, the VFS namespace's):
   usbd would wait for that thread, which waits for usbd. That is also why the automount of a new stick runs on a worker thread.

## Limitations and risks

* Not run on real silicon from here. QEMU's xHCI models are forgiving (no real timing, no TT, no link states, a full-speed-only
  hub, no USB 3 hub), so the high-speed hub with translators, USB 3 hubs (SET_HUB_DEPTH, warm reset paths, link states), CAS handling
  and 64-byte contexts are implemented from the specification and Linux's behaviour and exercised only by reading code.
* One interrupter, polled; ERDP is updated per batch. Event ring size 256, command ring 256.
* No isochronous endpoints, no streams, no LPM/U1/U2, no suspend, no port power budgeting (over-current is logged).
* Report-descriptor parsing is deliberately small (no feature reports, no consumer-page keys, no multi-button beyond three, no
  absolute devices). A keyboard that only has an NKRO bitmap interface without a boot interface is not bound.
* Control transfers are limited to 4096 bytes; configuration descriptors beyond 4096 bytes are truncated; at most 16 interfaces and
  8 endpoints per interface are recorded; at most 4 controllers.
* Devices whose setup fails four times are ignored until replugged. A device that times out once is marked broken and ignored.
* Mass storage: only SCSI over the bulk-only transport (no UAS, so no stream-based throughput; a UAS stick is driven through its
  bulk-only alternate setting, which is slower but works). Not tested against hardware, and QEMU shapes what was tested: its sticks have
  one LUN (`usb-bot` with a second LUN hangs the OVMF firmware before the kernel starts, so several real LUNs are exercised only through
  the empty-slot path, `usbstorage.fault=maxlun:2`), never NAK for long, and answer a missing command wrapper with a STALL. Timeouts are
  fatal for the stick (the core marks a device `broken` after one), so a very slow flash chip that takes more than 15 s for 64 KiB would
  be given up on; the timeouts are generous on purpose. After a bulk-only reset the xHCI data toggle is only reset by Reset Endpoint when a
  stall preceded it (QEMU does not model toggles; a real device that needs a reset without any stall may see a toggle mismatch).
* `disconnect` cannot release the `BlockDev` structs of a stick that had disks (see above); a machine that is plugged and unplugged
  thousands of times loses a few hundred KiB.
