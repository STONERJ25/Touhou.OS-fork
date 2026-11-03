# CPU temperature, frequency and power, and what can be learned about the fan

`cat /proc/thermal`, `sensors`, and for programs `gap_thermal_read()` (`<gap_thermal.h>` in libgap) show what the processor itself reports. **The fan is not in the CPU**: its speed is
read by another chip, and this version does not read it. What it has is a careful, opt-in way to find out which chip that is (`sudo sensors --probe-lpc`), and a plan.

## What is read, and how it is kept from hurting

All reads, no writes (`kernel/arch/x86_64/thermal.c`). Intel's registers (SDM volume 3 chapters 14.7, 14.9, 15.10 and volume 4):

| register | what | when it is tried |
|----------|------|------------------|
| `IA32_THERM_STATUS` 0x19C | core 0's digital sensor: bit 31 reading valid, bits 22:16 degrees *below* the throttling temperature, bits 30:27 resolution, and the status/log bit pairs (bit 0 thermal, 2 PROCHOT, 4 critical, 6 and 8 thresholds, 10 power, 12 current, 14 cross-domain limit; the odd bits are the sticky logs) | CPUID leaf 6 EAX bit 0 |
| `MSR_TEMPERATURE_TARGET` 0x1A2 | bits 23:16 TjMax (100 C on an i5-7500), bits 29:24 a programmed offset | read; if it does not answer TjMax is taken as 100 C and the output says `assumed` |
| `IA32_PACKAGE_THERM_STATUS` 0x1B1 | the same layout for the package | CPUID leaf 6 EAX bit 6 |
| `IA32_PERF_STATUS` 0x198 | bits 15:8 the multiplier the core runs at; times 100 MHz (133.33 MHz on Nehalem and Westmere; unknown on Atoms) | CPUID.1 ECX bit 7 |
| `MSR_RAPL_POWER_UNIT` 0x606, `MSR_PKG_ENERGY_STATUS` 0x611 | energy unit 1/2^bits(12:8) J (1/2^14 = 61 uJ), and the 32-bit package energy counter, from which the power follows | Intel family 6, model >= 0x2A and not an Atom, and both must answer |

Only the boot processor runs this kernel, so the output says **core 0 and the package**; the other cores' sensors are in registers of cores that never run, and they stay unreachable until there is SMP.
AMD processors (temperature behind the SMN interface), and anything whose CPUID does not offer leaf 6 bit 0 - **including QEMU** - give `thermal: no thermal sensor` and a reason.

**The guarded `rdmsr`** (`kernel/include/msrsafe.h`). A `rdmsr` of a register the CPU does not have raises #GP, and a #GP in the kernel is a panic; whether a register exists depends on the model, the firmware and
the hypervisor, and a CPUID bit that says "yes" is no promise (QEMU's TCG answers some registers and faults on others, and other hypervisors differ again). The kernel had no safe-MSR helper, so this adds one without touching
`trap.c` or `idt.c`: the one `rdmsr` instruction that `msr_read_safe()` executes has its address in a small table (`msrsafe.asm`), a stub reached through **vector 13 of the IDT** (patched at run time by `msrsafe_init()` after
checking that the gate is the interrupt gate `idt.c` builds; the old target is kept and every other #GP is chained to it with the stack untouched) looks the faulting address up in the table, and a hit
resumes at the function's `return -1` path. `msrsafe.test` on the command line logs a comparison with a plain `rdmsr` of EFER and runs a deliberate #GP (a bad segment selector) through the stub (`[Utsuho] msrsafe: a deliberate #GP was caught`); `msrsafe.crash` then executes one at an address the table does not list, which must reach the kernel's own handler and panic as ever.
A change in `trap.c`'s #GP path would do the same job with one hook; the run-time patch was chosen because `trap.c` is being edited elsewhere.

## `/proc/thermal`

`key: value` lines, stable names:

```
thermal: digital thermal sensor
source: cpu                           or: synthetic (thermal.fake= on the kernel command line)
vendor: GenuineIntel
cpu: family 6 model 0x9e stepping 9
cores: core 0 and the package (this kernel runs one core; the others' sensors cannot be read)
tjmax: 100
tjmax_source: msr 0x1a2               or: assumed (msr 0x1a2 did not answer)
tjmax_offset: 0
core0.valid: yes                      the sensor's "reading valid" bit; without it there is no temp/margin
core0.temp: 41                        tjmax - margin, degrees Celsius
core0.margin: 59                      the digital readout: degrees below the throttling temperature
core0.resolution: 1
core0.status: -                       or the limits active now: hot prochot critical threshold1 threshold2 power_limit current_limit cross_domain
core0.log: -                          the same names, for what happened since software last cleared the log (nothing here ever does)
package.valid: yes                    (the package.* lines are the same, from 0x1b1)
...
ratio: 34
bus_khz: 100000
mhz: 3400
energy_unit: 1/2^14 J
energy_raw: 0x01002000
energy_uj: 1024128906                 raw * 10^6 >> 14
power.window_ms: 5231                 the last measuring window of at least half a second ...
power.window_from_raw: 0x01000000     ... the counter at its ends ...
power.window_to_raw: 0x01002000
power.mw: 11400                       ... and the milliwatts (energy delta in uJ / window in ms; the counter is 32 bits and wraps)
msr.0x19c: 0x00000000883b0000         every register that was read, raw
```

`power.mw` is the average over the last *completed* window: the first window starts at the boot's reading of the counter and each next one at the end of the last, a window closes when a read finds it at least 500 ms old (reads of the
file come in pairs - `cat` reads again for the end of the file - so a shorter window would be noise), and a gap over 20 minutes is shown as `-` because the counter could have wrapped unseen. `sensors` reads twice, 0.6 s apart, to show a fresh number.
Without a sensor: `thermal: no thermal sensor` and `reason:` (the CPU is not Intel; no CPUID leaf 6; no digital sensor in it; the synthetic table has no register 0x19c).

## `sensors`

```
$ sensors
Core 0           41 C  (59 C below the limit)
Package          42 C  (58 C below the limit)
Limit (TjMax)   100 C
Frequency      3400 MHz  (34 x 100 MHz)
Package power   11.4 W  (average over 0.6 s)
Fan             not readable here: the fan is not on the CPU (man sensors; as root, sensors --probe-lpc looks for the chip that has it)
```

`sensors -r` is the kernel's text; status words appear in brackets after a line (`[hot power_limit]`); a synthetic table prints `(synthetic readings: thermal.fake= is on the kernel command line)` first, so that nobody takes it for the machine.
A machine without a sensor gets `no thermal sensor: <reason>` and status 1. In a program: `struct gap_thermal t; gap_thermal_read(&t);` (`user/libc/include/gap_thermal.h`) gives the same numbers parsed, which is all a panel widget needs.

## `thermal=off`: the escape hatch

The sensors are read at every boot (one look, for the log line and the start of the power window), and a remote machine that stops there needs a person. `thermal=off` on the kernel command line makes `thermal_init()` return first thing:
no register is read, the IDT is not patched (so no guarded read is possible either), `/proc/thermal` says `no thermal sensor` with the reason `thermal=off on the kernel command line`. The update tool can push a `cmdline.txt` (docs/UPDATE.md), so the escape needs no visit.

## `thermal.fake=`: the test hook

`thermal.fake=MSR:VALUE[+STEP],...` (hexadecimal, at most 16 entries, the command line holds 255 characters) replaces the CPU by a table: a register that is not in it behaves like one that raised #GP,
and an entry with a `+STEP` returns `VALUE + STEP * (reads so far)`, so that the energy counter moves. The tests use the i5-7500-like table worked out in `tests/case_thermal.py`:

```
thermal.fake=1a2:640000,19c:883b0000,1b1:883a0c03,198:2200,606:a0e03,611:1000000+2000
```

(TjMax 100, core 59 below = 41 C, package 58 below = 42 C with "hot" and "power_limit" set and logged, multiplier 34, energy unit 1/2^14 J.) A table that does not parse is ignored with a log line.

`thermal.force` is the other hook: it makes the code believe CPUID offered every sensor and read the six registers for real, through the guard, on a CPU that has none. QEMU's CPU is an `AuthenticAMD` one without leaf 6 bit 0, so
without it the thermal code stops at the vendor check and no `rdmsr` of these registers ever runs there; with it whatever the emulator answers (zeros, or a #GP) has to come out as a line of text (`core0.valid: no`, or `core0.error: msr 0x19c did not answer`), never as a panic. On real hardware it is of no use.

## The fan (second part: careful)

The usual homes of a fan tachometer on a PC are (a) the **Super I/O** chip's hardware-monitor block on the LPC bus (Nuvoton NCT67xx, ITE IT87xx, Winbond, Fintek, SMSC/Microchip), (b) an embedded controller, and (c) Dell's
SMM BIOS interface. **(c) is not used: an SMI from this kernel froze the Dell once** ([POWER_AND_TIME.md](POWER_AND_TIME.md)), and nothing here writes port 0xB2 or any other SMI trigger. (b) has no portable interface. (a) can be identified:

`sudo sensors --probe-lpc` (root; `echo probe > /proc/superio`) runs `kernel/arch/x86_64/superio.c` **once, when asked, never at boot, never by itself**, with interrupts off for the few hundred microseconds it takes. Exactly what it does,
for each configuration port P in {0x2E, 0x4E} (the data port is P+1):

1. reads registers 0x20 and 0x21 (an index write, a data read each) with no key: what a quiet bus or a device that needs no key says;
2. **Nuvoton / Winbond / Fintek**: writes `0x87, 0x87` to P, reads 0x20/0x21 again, writes `0xAA` to P (lock);
3. **SMSC / Microchip**: writes `0x55` to P, reads the ids, writes `0xAA` to P;
4. **ITE**: writes `0x87, 0x01, 0x55, 0x55` to P (`0x87, 0x01, 0x55, 0xAA` when P is 0x4E), reads the ids, and *only if the id is an ITE one in the table* locks with `0x02` to P and `0x02` to P+1;
5. for a chip that is in the table and has an ordinary hardware-monitor logical device (Nuvoton/Winbond/Fintek 0x0B or 0x04, ITE 0x04): writes `0x07` to P and the logical device number to P+1 (the only other data-port write), reads register 0x30
   (activation) and 0x60/0x61 (its I/O base address);
6. if a key made the registers say something not in the table, a final `0xAA` to P (an index write, harmless).

The order is not an accident: ITE's key contains SMSC's whole key (0x55), so SMSC is tried first and a chip that has been recognised is never sent another family's key; Nuvoton's key is the start of ITE's, which a chip
waiting for the rest simply restarts on. `tests/host/test_thermal.py` runs the probe against a model of the chips and checks every byte it writes: nothing but the ports above, no data-port write at all on an empty bus or
a device that ignores the keys (an embedded controller with an index/data pair at 0x4E/0x4F, say), and every chip back in its locked state. A device that merely sits at 0x2E/0x4E sees index writes of key bytes and register numbers and
nothing else.

The result is in `/proc/superio` (`superio: nothing answered`, or per port the family, the id, the chip name, and `hwm: logical device 0x0b, active, io base 0x0a30`) and one log line. The names in the id table (`superio.c`) are written from
memory of the vendors' datasheets and are **not checked against hardware**; the raw id is always printed.

**What it does not do: read a fan.** Even for a chip it knows, `fans: not read` stays in the report. Reading the tachometer takes registers in the hardware monitor's own index/data pair (at the base address found in step 5) -
for the Nuvoton parts through a *bank select*, which is a write to a register the firmware may be using at the same moment - and those registers and the count-to-RPM formula must be taken from the chip's datasheet, which was not available
offline. What the lead should fetch once `--probe-lpc` has named a chip: for a **Nuvoton NCT6791D/6792D/6793D/6795D/6796D/6797D/6798D** the datasheet chapter "Hardware Monitor" (the register map by bank, the FANIN count/RPM registers, the bank select register 0x4E,
the logical device 0x0B base address); for an **ITE IT8620E/IT8628E/IT8728F** the "Environment Controller" chapter (the fan tachometer reading registers - believed to be 0x0D-0x0F with 16-bit extensions - the fan divisor and 16-bit mode registers, and where the index/data pair sits relative to the base address - believed to be base+5 and base+6; these were written from memory and are exactly what must be checked);
for an **SMSC/Microchip SCH5xxx** (the likely family in a Dell business desktop) the datasheet's runtime register block and, since those chips are mostly behind Dell's own firmware, whatever Dell's service documentation says about it.
A clear "this machine's fan cannot be read yet, here is what was found" is the result this part promises.

## Tests

- **Host** (`python tools/hosttest.py discover -s tests/host -p "test_thermal.py" -v`, 25 tests): the decoding of the status registers for 2000 random values against bit arithmetic, TjMax, the flag names, the reference clocks per model, the RAPL units and the power from a wrapping counter,
  the `thermal.fake` grammar, and the Super I/O probe against the chip model (an NCT6793D on 0x2E, an IT8728F on 0x4E, SMSC, an unknown ITE id, an empty bus, an embedded controller that ignores keys, two chips at once; every byte checked).
- **QEMU** (`python tools/qemu-slot.py -- python tests/run_all.py thermal_`, six tests): `thermal_forced_reads_never_fault` (the real `rdmsr`s under the guard on a CPU without them), `thermal_off_touches_nothing`, `thermal_a_gp_outside_the_table_still_panics` (the stub chains to the kernel's handler), `thermal_no_sensor_and_the_guarded_read` (QEMU has no sensor: the files say so; the guarded `rdmsr` of EFER equals a plain one; a deliberate #GP is caught; the probe is refused to a user, finds an empty bus for root),
  `thermal_fake_registers` (every key of `/proc/thermal`, the power identity across two reads, the whole text of `sensors`), `thermal_fake_partial_table` (assumed TjMax, an invalid core reading, no frequency or power).

## The plan for the Dell (the lead's)

1. After the next push: `dmesg | grep Utsuho` - expect `thermal: core 0 at 3x C, package at 3x C (TjMax 100 C), 800 to 3800 MHz, energy counter present` (no `[synthetic]` marker). `thermal: no thermal sensor` would mean the firmware hides CPUID leaf 6 (say so).
2. `sensors` and `cat /proc/thermal` at idle and again under load (`yes > /dev/null &` for a minute, then `kill %1`): idle 30 to 45 C, the package a degree or two above core 0, `ratio` rising from 8 towards 34 to 38 (the i5-7500 turbos to 3.8 GHz),
   `power.mw` from about 3000 to 15000 at idle and up to the 65 W limit under load. `sensors -r` is the raw dump; the register values can be checked by hand against the layouts above. **Send the output of `sensors -r`** whatever it looks like.
3. Only if the lead agrees, as root: `sensors --probe-lpc`, then `dmesg | tail -5` and `cat /proc/superio`. Expected: `nothing answered` (then the fan is on the embedded controller, which is out of reach for now), or a chip name and a base address (then the datasheet pages above are the next job).
   It has **not** run on any real machine. It writes key bytes to ports 0x2E/0x4E and 0x2F/0x4F only; if the machine freezes at that moment, the bytes are listed above and a freeze there would be the first sign that Dell's firmware traps those ports with an SMI.
4. The temperature at idle is also what to watch during a long run: the status words (`hot`, `prochot`) appear in `sensors` the moment the CPU throttles.

## Files

`kernel/arch/x86_64/thermal.c` (the registers, `/proc/thermal`, `/proc/superio`), `thermal_core.c` and `kernel/include/thermal.h` (decoding and units, host-tested), `msrsafe.asm`, `msrsafe.c` and `kernel/include/msrsafe.h` (the guarded rdmsr),
`superio.c` and `kernel/include/superio.h` (the probe), `user/bin/kappabox/sensors.c`, `user/libc/src/gap_thermal.c` and `user/libc/include/gap_thermal.h`, the manual page `sensors(1)`, `tests/case_thermal.py`, `tests/host/test_thermal.py`.

## State when paused / how to resume (2026-11-03, the owner's budget ran out)

**Done, committed and tested on `agent/clock-sensors`:** the CPU readout (Part 2.1 of the brief) - `/proc/thermal`, `sensors`, `gap_thermal_read()`, TjMax, the multiplier, the RAPL energy counter and the power from it, the guarded `rdmsr` (the #GP stub),
the test hooks (`thermal.fake=`, `thermal.force`, `thermal=off`, `msrsafe.test`, `msrsafe.crash`). 25 host tests and six QEMU tests (`thermal_*`) pass; so do the boot, panic, hardware-audit and power tests that run the same boot path.
The opt-in Super I/O probe (Part 2.2) is also written, host-tested against a model of the chips (every byte it writes is checked) and tested in QEMU on an empty bus - but it has **never run on a real machine**.

**Not done / open:**
- **The fan is not read.** Nothing reads a tachometer; even for a chip in the table the report says `fans: not read`. It waits for the datasheet pages listed under "The fan" and for a first `--probe-lpc` on the Dell to say which chip it is
  (an ITE chip's monitor has no bank register and would be the easy case; a Nuvoton one needs the bank select, i.e. a write to a register the firmware may be using). If the Dell's fan sits behind its embedded controller or Dell's SMM interface (not usable: an SMI froze this kernel once), "cannot be read" stays the answer.
- Core 0 and the package only; per-core sensors wait for SMP. AMD is not supported (another interface). The names in the Super I/O id table are from memory.
- Never tried: the real registers of a real CPU (QEMU has none; its CPU model answers zeros, which the `thermal.force` test shows are handled). The Dell is the first real test of `/proc/thermal`, and its boot is the first real test of the IDT patch outside QEMU (`thermal=off` is the way out if the boot stops at a `[Utsuho]` line).
- Ideas: a panel widget on `gap_thermal_read()`; a log line or warning when `hot`/`prochot` appears; RAPL's other domains (cores, graphics, memory); `APERF`/`MPERF` for the real frequency.

**To resume:** `git switch agent/clock-sensors`; `python tools/build.py`; host `python tools/hosttest.py discover -s tests/host -p "test_thermal.py" -v`; QEMU `python tools/qemu-slot.py -- python tests/run_all.py thermal_`. Then steps 1 to 4 of the Dell plan above, and send back `sensors -r`.

## Seen on the Dell (2026-11-03, build 3fdee2b, idle)

`sensors`: Core 0 32 C (68 C below the limit), Package 34 C (66 C below), TjMax 100 C, 800 MHz (8 x 100 MHz), package power 3.7 W averaged over 0.6 s. The fan line says it is not readable here, as designed. The registers answered on the real i5-7500 without a fault.
Not tried: `sudo sensors --probe-lpc` (the opt-in look at the Super I/O chip), readings under load.
