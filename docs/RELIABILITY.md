# Recovery without a person: the watchdog, the panic record, the netconsole

The owner's Dell is updated over the network ([UPDATE.md](UPDATE.md)). Until now a kernel that hung or panicked needed someone at the
machine: to read the screen, and to press the reset button twice so that the loader's fallback brought the previous kernel back. Three
pieces close that gap and make failures visible from another computer:

| Piece | Who (log tag) | What it does | Command line |
|---|---|---|---|
| hardware watchdog | Momiji | the Intel TCO timer restarts a hung machine; armed by the loader, fed by the kernel, then by init | `wdt=SECONDS` (default 120), `wdt=off` |
| panic record | Akyuu | a panic's text kept in RAM across the restart, read at the next start; a panic restarts the machine by itself | `panic=SECONDS` (default 30 on a trial start, off otherwise), `persist=` |
| netconsole | Aya | every kernel log line as a UDP datagram; a panic's text through the card's polled transmit | `netconsole=IP[:PORT]` |

Momiji Inubashiri is the white wolf tengu who keeps watch on Youkai Mountain; Hieda no Akyuu remembers in each life what the one before saw;
Aya Shameimaru is the reporter who gets the news out first.

## The hardware watchdog

### The timer

The TCO timer of Intel chipsets (the ICH9 datasheet and the 100/200-series PCH datasheets describe it; `shared/tcohw.h` has the registers)
counts down in 0.6 s ticks from `TCO_TMR` (10 bits). A write to `TCO_RLD` reloads it: that is "feeding" it. When it reaches zero the first
time, `TCO1_STS.TIMEOUT` is set (and an SMI raised if `SMI_EN.TCO_EN` is set) and it counts again; at the second zero `TCO2_STS.SECOND_TO_STS`
is set and the chipset resets the platform, unless the `NO_REBOOT` bit is set. So `wdt=SECONDS` is programmed as two countdowns of half
that each; the longest the hardware can count is 1227 s. `SECOND_TO_STS` survives the reset it caused, and the next start reads it:
`[Momiji] watchdog: the previous start ended in a watchdog reset (TCO2_STS 0x6)`.

Where the registers are depends on the chipset generation, and the driver finds them through PCI configuration space:

| Chipset | TCO registers | NO_REBOOT |
|---|---|---|
| ICH6 .. 9-series PCH, and QEMU's q35 (an ICH9) | PMBASE (LPC bridge 00:1f.0, config 0x40) + 0x60 | GCS bit 5, memory-mapped at RCBA (LPC config 0xf0) + 0x3410 |
| 100/200-series PCH (Sunrise Point, Union Point: the Dell OptiPlex 7050's Q270, LPC `8086:a2c6`) | TCOBASE in the SMBus controller (00:1f.4, config 0x50, decoded while TCOCTL bit 8 is set) | bit 1 of the SMBus controller's General Control register in the PCH private configuration space: SBREG_BAR of the P2SB bridge (00:1f.1, which the firmware hides: config 0xe1 bit 0 unhides it for the moment it takes to read the BAR) + (0xc6 << 16) + 0x0c |
| 300-series and later | found by the second rule | moved again: reported as unknown |

### Who feeds it

1. **The loader** (`boot/recovery.c`) arms it as its very last step, after ExitBootServices (no firmware code can stop it then), with
   `wdt=SECONDS` from the command line: a kernel that hangs at its first instruction restarts the machine. It does that only for a kernel
   image that carries the mark `Gensokyo:TCO-watchdog-feeder:1` (`BOOT_WDT_MARK`, in `tcowdt.c`): an older kernel, after a fallback to
   one, would not feed it and would be restarted every two minutes. For such a kernel, and with `wdt=off`, a timer that is still running is
   stopped. What the loader did goes to the kernel in `BootInfo.wdt_*` (a PC has no serial port to show the loader's own lines).
2. **The kernel** (`kernel/drivers/tcowdt.c`, an init call) finds the timer, reports what the previous start's reset was, clears
   `NO_REBOOT` where the hardware lets it, (re)arms the timer and feeds it from the `Momiji` thread (every quarter of the timeout, at most
   5 s): a kernel whose scheduler stops lets it run out. With an older loader (BootInfo version 4) the kernel arms it here, so a hang
   before this point is not covered. The storage policy runs before the init calls, and the check and the repair of the Human Village
   (`hvfsck=safe`, [STORAGE](STORAGE.md) "Mending at boot") can take longer than the timer, so they reload it themselves
   (`tcowdt_boot_feed()`, from every request into the device) until the thread exists.
3. **User space**: init starts its feeder (`wdt-feeder`, `user/sbin/init.c`) first thing; it opens `/dev/watchdog` and writes a byte
   once a second. From the open on, the kernel no longer feeds: a user space that cannot run its feeder (a hung system) lets the timer run
   out. `SIGTERM` makes the feeder close with the magic `V` (the watchdog stops on purpose); if it dies of anything else, init starts it
   again and the new one goes on feeding the running timer. The update tool's long ESP phase changes nothing: the feeder is its own process.
4. **Nobody**: after `/dev/watchdog` was closed without the magic `V` (the machine restarts within the timeout unless it is opened and fed
   again); during a shutdown; in a test.
5. **A restart or poweroff that was asked for** (`sys_reboot`, `tcowdt_shutdown()`) takes the timer away from its feeder and gives the
   shutdown a budget of its own, 300 s (or the timeout, if that is longer), so that a slow sync or ESP unmount is not cut short, while a
   shutdown that hangs still ends in a restart. The panic path does the same just before it restarts the machine.

`/dev/watchdog` (mode 0600) follows the Linux protocol: any write feeds the timer, and a `V` in the last write before the last close stops
it ("magic close"); there are no ioctls. A stopped watchdog is not armed again by opening the device; root writes `start` to `/proc/watchdog`.

### /proc/watchdog

```
chipset:  ICH6..9-series: PMBASE + 0x60, NO_REBOOT in GCS (RCBA + 0x3410), LPC 8086:2918
tco:      io 0x660, ACPI block io 0x600, NO_REBOOT: bit 5 of the register at 0xfed1f410
state:    armed, 120 s, 120 s left if nobody feeds it
feeder:   user space (/dev/watchdog): 'wdt-feeder' (pid 2), last fed 413 ms ago
reset:    possible (NO_REBOOT is clear)
noreboot: clear when the kernel looked
smi:      no SMI on the first timeout
late:     0 first timeouts noticed
loader:   armed it before the kernel started (120 s)
previous: no watchdog reset (TCO2_STS 0 at boot)
```

Root may write: `stop`; `start [SECONDS]` (fed by the kernel, or by user space if `/dev/watchdog` is open); `timeout SECONDS`; `test`
(nobody feeds it from now on: the machine must restart within the timeout). A second timeout that does not restart the machine, for
whatever reason, is noticed (the thread sees `SECOND_TO_STS` while it still runs) and reported:
`[Momiji] watchdog: the timer ran out twice and the machine did not restart: the reset is blocked (...)`, and `reset: BLOCKED` here.

Command line: `wdt=SECONDS` (2 to 600; default 120; values below about 5 s are for the tests: some chipsets ignore the smallest counts),
`wdt=off` (the hardware is found and reported, nothing is armed, no `/dev/watchdog`, no feeder), `wdt.nosmi` (clear `SMI_EN.TCO_EN` unless
`TCO_LOCK` is set), `wdt.keepnr` (leave `NO_REBOOT` as the firmware set it).

## The panic path and the record

`panic()` and `panic_frame()` (fatal exceptions, `trap.c`) print the incident as before (message, task, registers, backtrace) and then
`panic_end()` (`kernel/panic.c`):

1. says what happens next (`[Akyuu] the machine restarts in 30 s (panic=30: the default on a start under trial)`, or `the machine stays
   like this; the watchdog restarts it within 87 s ...`) and writes **the record**: the last 60 KiB of the kernel log, the panic text at its end;
2. lets the netconsole send what it has not sent yet, through the card's polled transmit;
3. after `panic=SECONDS` (default 30 on a start under trial, `update.trial=N` on the command line; off otherwise) restarts the machine
   (`machine_reboot`, the usual order of ways). A panic during an update trial therefore ends in the loader's fallback without a person.
   Off means the screen stays for whoever looks; an armed watchdog still restarts the machine when its timeout runs out
   (`wdt=off panic=0` keeps a panic on screen for good).

A second panic inside these steps (a fault in the netconsole's path, say) prints one line and goes on with the next step; a third only stops.

**The region.** The loader takes 64 KiB of RAM at `0x3f00000` (`BOOT_PERSIST_PHYS`; `persist=0xADDR`, page aligned and below 4 GiB, picks
another place; `persist=off` none) from the firmware with `AllocatePages(AllocateAddress)` and hands it to the kernel as `BOOTMEM_RESERVED`,
so no kernel (an older one included) gives it out; its contents are never touched by the loader. The kernel maps it uncached: a write is in
DRAM, not in a cache line, when the machine resets. Layout (`Rec` in `panic.c`): a header (magic `AKYUREC1`, the start's number, its state,
the last sign of life, who fed the watchdog, the panic's own line, when the kernel was built, a CRC-32) and at 4 KiB the text with a CRC-32
of its own. While the system runs the header says "running" and a heartbeat (the `Akyuu` thread) updates the last sign of life every 5 s;
a restart or poweroff that was asked for marks it "shut down"; a panic marks it "panic".

**What the next start says** (the lines go into the kernel log, so into `TOUHOU.LOG` and the netconsole too):

| The previous start ... | Log |
|---|---|
| panicked | `[Akyuu] previous panic: requested panic (start #2, on trial, 3 s after it began, kernel built Nov  3 2026 00:12:40)`, then the panic's lines from its `*** INCIDENT` header on, each as `[Akyuu]   \| ...`, then `that start's log up to the panic (its last 60 KiB) is in /proc/panic` |
| was shut down on request | `[Akyuu] the previous start (#3, kernel built ...) ended with a restart or poweroff that was asked for, 41 s after it began` |
| neither | `[Akyuu] the previous start (#2, ...) ended without a shutdown or a panic: its last sign of life came 95 s after it began (a hang that the watchdog or a person ended, or a power cut)` |
| left no record | `[Akyuu] the record region at 0x3f00000 holds no record of the previous start (the first start with it, a power cut, or firmware that clears memory at a restart)` |

`/proc/panic` shows the policy, the region, the previous start and the end of its log (a /proc file holds 16 KiB). Root may write `panic`
(a deliberate panic) or `pf` (a page fault in kernel mode: the fatal-exception path) to it: the tests and the checks on the Dell use that.
`crash=panic` on the command line (main.c) panics during the start, before the init calls; `kill -9 1` as root panics in init's exit.

## The netconsole

```
netconsole=192.168.1.15:6666                      the short form; the port is 6666 if it is left out, the source port 6665
netconsole=6665@/eth0,6666@192.168.1.15/          Linux's form: [SPORT]@[SIP]/[DEV],[PORT]@IP/[MAC]; SIP is ignored (the card's own
                                               address is used), MAC skips ARP for the next hop
netconsole.rate=500                            at most this many lines a second (the default)
python tools/netconsole.py --port 6666         on the development PC: prints each line with its time and sender, appends it to
                                               build/netconsole-YYYYMMDD.log (--out FILE, --bind ADDR, --quiet)
```

Every line of the kernel log is one UDP datagram, plain text (the screen's escape sequences taken out). Lines logged before the network
came up are still in the kernel's log ring (128 KiB) and go first. The `Aya` thread takes the network lock every 10 to 50 ms, waits for a
route to the target and for the next hop (the target, or the gateway) to answer ARP (nothing is sent before: ARP's queue keeps only two
packets; one greeting datagram, `-- netconsole: the TouhouOS kernel log follows --`, makes the stack ask, again every second and after ten
tries every 10 s), and sends through `ip_output()`. A card whose transmit ring is full gets the same line again on the next round.

**In a panic** there is no lock to take and nobody to wait for. `netcons_panic_flush()` builds whole Ethernet frames itself, for the card
and the next hop's MAC address that the ordinary path last used (the broadcast address if ARP never answered), and hands them one after the
other to the driver's polled transmit, `NetDev.netpoll_tx`: no lock, no allocation, no log line, interrupts off, at most 20 ms of waiting
per frame, and after three frames in a row that the card did not take (a stuck transmit unit) no more tries. `e1000.c` (82540EM, 82574L and the PCH parts I217/I218/I219: the Dell's card) and `virtio_net.c` have one; `rtl8139.c` and
`r8169.c` do not (the panic is then not sent, and the screen says so). At most the newest 32 KiB go out in a panic.

**Floods and loops.** At most `netconsole.rate` lines a second leave (a token bucket, one second's worth of burst); when the log runs more
than half its ring ahead, the netconsole skips to the newest 8 KiB and says so on the wire: `-- netconsole: 15214 lines (1342116 bytes) of the
log dropped: it came faster than 400 lines a second --`; lines the ring overwrote before they could leave are reported the same way. Nothing in
the sending path writes to the kernel log: a netconsole that logged its own trouble would send that line too, and log the trouble of sending
it. Its one line is the one at the start (`[Aya] netconsole: the kernel log goes to 192.168.1.15:6666 ...`); everything else is in
`/proc/netconsole` (target, state, card, counters, backlog). Root can write `flood N` there: N lines into the log at once (the tests' flood).

## Failure modes

| What goes wrong | What happens | What it looks like |
|---|---|---|
| The BIOS (or a pin strap) keeps `NO_REBOOT` set | the timer runs out twice and the machine goes on running | `NO_REBOOT is set and stays set ...` at boot, `reset: blocked` in /proc/watchdog; under `test`, `the reset is blocked` after the timeout |
| The firmware's SMI handler swallows the first timeout (`SMI_EN.TCO_EN` set, the handler reloads the timer) | the timer never reaches its second zero | `the first timeout raises an SMI ...` at boot; `test` never restarts and never reports a blocked reset. Try `wdt.nosmi` |
| The TCO block is not where the driver looks, or not decoded | no watchdog | `no Intel TCO watchdog (...)`: the reason names the register that was not set up |
| The firmware clears memory at a restart (`MemoryOverwriteRequestControl` set, left by Windows; a memory test; a restart through a full power cycle) | the record is gone | `holds no record of the previous start` after a panic; the netconsole and `TOUHOU.LOG` still have the panic |
| The firmware uses the region's memory in this start | no record in this start | `no panic record region (the firmware used that memory in this start)`; `persist=0xADDR` moves it |
| The NIC is what panicked, or its state is broken | the polled transmit waits 20 ms per frame; after three frames the card did not take, the panic path stops sending | the record was written before the netconsole is tried, and the restart comes after it; the screen says `the card does not take the frames` |
| A panic before the network was ever up, or before the init calls | nothing is sent | the record (mapped on the spot, if the init calls had not mapped it yet) and the screen |
| The loader is older than all this (BootInfo version 4: the Dell's loader until it is pushed with `--loader`) | no region, no arming before the kernel | `no panic record region (the loader is older than the record ...)`; the kernel arms the watchdog in its init calls |
| A fallback to a kernel from before the watchdog | the loader does not arm it for that kernel, and stops a running timer | `this kernel does not feed the TCO watchdog: it is not armed` (loader) |

Not covered: a hang in the firmware's own start, before the loader runs; `reboot`/`poweroff` in the kernel shell and the power button
without a desktop, which do not go through `sys_reboot` (the feeder keeps feeding through their sync, so nothing is cut short, but the record
then says "ended without a shutdown"); `/dev/watchdog` ioctls; NO_REBOOT on 300-series and later chipsets; a polled transmit for `rtl8139.c`
and `r8169.c`; `/proc/panic` shows the last 16 KiB of the 60 KiB record (the next start's log has the panic's own lines).

Two things QEMU does differently from a PC, which the tests rely on or work around: its TCO timer goes on counting through a guest reset (a
PC's platform reset stops it; this is why the panic path and the shutdown give the timer the long budget before they restart), and it lets
software clear GCS's `NO_REBOOT` even when its strap (`-global ICH9-LPC.noreboot=true`) blocks the reset, so the blocked reset is only found
when the timer has run out.

## Hardware test plan for the Dell (for the lead)

Nothing here has run on the Dell. In order; each step says what to expect and what the alternative means. Have the USB stick at hand for
step 4. On the development PC, before step 1: `python tools/netconsole.py --port 6666` (allow it through the Windows firewall), and note
the PC's LAN address (`ipconfig`), here `192.168.1.15`.

1. **Look, do not arm.** Push the new kernel and initrd (not the loader) with the command line in use plus
   `wdt=off netconsole=192.168.1.15:6666` (`--cmdline "..."`; `cat /proc/cmdline` shows the words in use, without the loader's `update.*`).
   Confirm it. Expected: the receiver shows the whole boot log; `dmesg | grep -E 'Momiji|Akyuu|Aya'` shows
   `[Momiji] TCO watchdog: 100/200-series PCH: TCOBASE in the SMBus controller, NO_REBOOT behind the P2SB bridge; LPC 8086:a2c6, TCO at io 0x400, ACPI block at io 0x1800`
   (the TCO base is what the BIOS chose: 0x400 is usual; the ACPI block must be 0x1800, which the FADT says too), `wdt=off: the watchdog is not armed`, and
   `no panic record region (the loader is older than the record (push it with --loader))`. Write down `cat /proc/watchdog`: the `noreboot:` line
   (set or clear as the firmware left it), `smi:` and `tco:` (the NO_REBOOT register should be `0xfdc6000c` if SBREG_BAR is the usual `0xfd000000`).
   `cat /proc/pci` must still not list 00:1f.1 (the P2SB is hidden again). If the TCO is not found, the reason is the line to send back.
2. **Arm it and let it fire.** Push (or edit `cmdline.txt` to) `wdt=30 netconsole=192.168.1.15:6666`, confirm. Expected:
   `the watchdog is armed: 30 s, armed now (the loader is older ...)`, `'wdt-feeder' (pid N) feeds the watchdog ...`, and either nothing about
   NO_REBOOT (it was clear) or `NO_REBOOT was set: cleared`. Then, over ssh, `sudo sh -c 'echo test > /proc/watchdog'`. Expected: the machine
   restarts within 30 s (the receiver's last lines are `watchdog test: ...` and `nobody has fed it for ...`), and the next start logs
   `[Momiji] watchdog: the previous start ended in a watchdog reset (TCO2_STS ...)`. If it does not restart: after about 35 s the receiver shows
   `the reset is blocked (...)`; the reason in brackets says which: `NO_REBOOT stays set` (look in the BIOS setup for a watchdog option, or accept
   that the Dell cannot), or `the first timeout raises an SMI` (try once more with `wdt.nosmi`). If the start after the restart does not mention the
   watchdog reset, the BIOS clears `SECOND_TO_STS` on its way (the record of step 4 then tells instead). Do this on a confirmed start, not on a
   trial start: a watchdog restart during a trial counts as a failed start.
3. **A hung user space.** With `wdt=30`: `sudo kill -STOP PID` with the feeder's pid from `/proc/watchdog` (`'wdt-feeder' (pid N)`). Expected: a restart within
   30 s, the same line at the next start. This is the case the default policy is for.
4. **The loader** (`push-update.py --loader`, the risky one: the stick at hand). Expected in the kernel log of the next start:
   `[Momiji] the watchdog is armed: 120 s, the loader armed it, the kernel takes it over ...` and `/proc/watchdog` `loader: armed it before the
   kernel started (120 s)`; `[Akyuu] the record region at 0x3f00000 holds no record of the previous start (...)` on the first start with it. If
   `/proc/panic` says `record: none (the firmware used that memory in this start)`, try `persist=0x10000000`.
5. **Does the Dell keep RAM through a restart?** With `panic=10` added: `sudo sh -c 'echo panic > /proc/panic'`. Expected: the receiver shows the
   panic (`*** INCIDENT: KERNEL PANIC ***`, `requested through /proc/panic ...`, the backtrace, `the machine restarts in 10 s (panic=10)`: this is
   the I219's polled transmit at work), the machine restarts by itself (through the PCI reset port, the way that works on the Dell) and the next
   start logs `[Akyuu] previous panic: requested through /proc/panic by 'sh' ...` with the panic's lines. `holds no record` instead means the
   firmware cleared or retrained the memory (or `MemoryOverwriteRequestControl` is set: the loader's line says so on a serial console only, the
   kernel's line mentions it): report it; then only the netconsole and `TOUHOU.LOG` keep a panic. Then step 3 again: does the record survive a
   *watchdog* reset too (`the previous start ... ended without a shutdown or a panic`)? A TCO reset may go through a full power cycle.
6. **The whole point.** Push a kernel that hangs at its first instruction (`new_system("x", hang=True)` in `tests/case_update.py` makes one from
   `build/esp`) with `--no-confirm`. Expected: without anyone touching the machine, it comes back on the previous kernel within about 2 x (120 s
   + the firmware's start); `update status` says "the PREVIOUS kernel and initrd" and the log has `the previous start ended in a watchdog reset`.
   Then `sudo update rollback`.

## Tests

`python tests/run_all.py reliability_` (`tests/case_reliability.py`):

| Test | What it shows |
|---|---|
| `reliability_wdt_feeders` | the loader arms the timer, the kernel takes it over, init's feeder takes it from the kernel; /proc/watchdog says all of it; a running user space is never restarted |
| `reliability_wdt_hung_userland_restarts` | a stopped feeder (SIGSTOP) lets the timer run out: the machine restarts within the timeout |
| `reliability_wdt_kernel_feeds_until_user_space_does` | without a user-space feeder (`init=kshell`) the kernel feeds it for good |
| `reliability_wdt_magic_close_and_controls` | SIGTERM: the feeder's magic close stops the watchdog; `start`, `echo V > /dev/watchdog`, and a close without the `V` that restarts the machine |
| `reliability_wdt_blocked_reset_is_reported` | QEMU's NO_REBOOT strap: `test`, two timeouts, no restart, "the reset is blocked" |
| `reliability_wdt_shutdown_gets_time` | `reboot` gives the shutdown 300 s |
| `reliability_wdt_off` | `wdt=off persist=off`: nothing armed, no /dev/watchdog, no feeder, no region; both say why |
| `reliability_record_watchdog_reset_and_clean_restart` | real resets: a watchdog reset reported by the TCO status and by the record (last sign of life); a clean restart recorded as such |
| `reliability_panic_record_survives_the_restart` | a panic and a kernel page fault, each restarted after `panic=2`; the next start logs "previous panic: ..." with the backtrace and the registers; /proc/panic holds the end of the log; the watchdog does not fire during the restart |
| `reliability_panic_reaches_the_log_file` | the start after a panic puts the record's lines into its log, and so into `TOUHOU.LOG` on a HUMANVILLAGE stick |
| `reliability_panic_on_trial_falls_back` | an update on trial whose kernel panics at every start: 30 s each (the default under trial), and the loader's fallback, which reports the panic |
| `reliability_wdt_hung_trial_kernel_falls_back` | a pushed kernel that hangs at its first instruction: the loader's watchdog restarts it twice and the previous kernel returns, nobody pressing anything |
| `reliability_record_region_elsewhere` | `persist=` at an address that is not RAM, and at another free one |
| `reliability_netcons_lines_and_panic` | e1000: the early lines, new lines, and a panic (root kills init) through the polled transmit |
| `reliability_netcons_virtio_exception` | virtio-net's polled transmit, with a fatal exception's registers |
| `reliability_netcons_flood_and_no_loop` | 20000 lines at once: the rate holds, the drop is reported on the wire, a later line still arrives, the netconsole never logs about itself |
| `reliability_netcons_unreachable_is_quiet` | a target that never answers ARP: the netconsole waits, sends nothing it would lose, logs nothing, and the network works |

`python tools/hosttest.py discover -s tests/host -p "test_tcohw.py" -v` (no QEMU; WSL on Windows) runs `shared/tcohw.h` itself against
simulated chipsets (`tests/host/tcohw_sim.c`): q35's ICH9, and the Dell's Q270 path that QEMU cannot run: TCOBASE `0x400` and ABASE `0x1800` found,
the NO_REBOOT register at `0xfdc6000c` bit 1, the P2SB hidden again with exactly the writes `P2SBC = 0` and `P2SBC = 0x100` (also when the hidden
bridge reads as all ones), NO_REBOOT cleared without touching its neighbours, a TCO block whose decoding is off, a chipset that is not Intel,
and the tick arithmetic.

## Files

`shared/tcohw.h` (the TCO hardware, loader and kernel), `shared/bootinfo.h` (BootInfo version 5: `persist_*`, `wdt_*`, `BOOT_WDT_MARK`),
`boot/recovery.c` (and three calls in `boot/main.c`), `kernel/drivers/tcowdt.c`, `kernel/include/tcowdt.h`, `kernel/panic.c`,
`kernel/include/panicrec.h`, `kernel/drivers/netcons.c`, `kernel/include/netcons.h`, `NetDev.netpoll_tx` (`kernel/include/net.h`) with its
implementations in `kernel/drivers/e1000.c` and `kernel/drivers/virtio_net.c`, `panic_frame()` in `kernel/arch/x86_64/trap.c`, the
shutdown hook in `sys_reboot()` (`kernel/sys/syscalls.c`), the feeder in `user/sbin/init.c`, `tools/netconsole.py`, `tests/case_reliability.py`.
