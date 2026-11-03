# The PC-98 HLE BIOS (milestones M2 and M3 of the project plan)

The PC-98 machine of `user/lib/pc98` ([PC98.md](PC98.md)) now boots the owner's real hard-disk images to an MS-DOS prompt:
the disk's own IPL, NEC's boot menu, the partition boot sector, `IO.SYS`, `MSDOS.SYS` and `COMMAND.COM` run unchanged, and
`AUTOEXEC.BAT` starts its programs. There is no BIOS ROM: a **high-level-emulation (HLE) BIOS** answers the calls in C.
This page says how it works, what every call returns, which work-area bytes it fills, what the boot chain was seen to
need, and what is missing. Files: `user/lib/pc98/include/pc98bios.h`, `user/lib/pc98/src/pc98bios.c`, the machine hooks in
`pc98mach.c`/`pc98.h`, the harness `tests/host/pc98run.c` with `tools/pc98run.py`, the tests `tests/host/test_pc98bios.py`.

## How it works

- **The ROM page.** `pc98_bios_init()` maps a 10 KiB page of our own at FD800-FFFFF (segment FD80h, where the real BIOS
  sits) and points all 256 interrupt vectors into it. The stub for vector *n* is at FD80:(4*n): `INT n; IRET; NOP`. Every
  other byte of the page is `INT3`.
- **The trap.** The CPU core calls `X86Bus.soft_int` for every `INT n`. The machine passes it to `pc98_bios_soft_int()`,
  which looks at where the instruction is: inside the ROM page it is the BIOS's own stub, and the call is answered in C
  (registers in `m->cpu`, guest memory directly); anywhere else the CPU dispatches through the vector table as usual.
  Because the stub is reached *through the vector*, the BIOS answers all the ways a program gets there: `INT 1Bh`, a
  driver that hooked the vector and chains with `PUSHF; CALL FAR [old vector]`, or a hardware interrupt. No change to the
  CPU core was needed.
- **Results in FLAGS.** A call that returns CF sets it in the caller's FLAGS image on the stack (SS:SP+4 when the stub's INT
  runs); the stub's IRET pops it.
- **Waiting.** A call that cannot finish (INT 18h AH=00h with an empty key buffer) points IP back at the stub's INT, sets
  IF and halts the CPU. The next interrupt (the keyboard) runs, its IRET re-executes the INT, which asks again.
- **Calling the guest.** Where the BIOS has to call guest code (the STOP key's INT 06h, the interval timer's INT 07h), it
  enters that vector from the stub (`x86_interrupt`), so the guest's IRET returns to the stub's IRET.
- **Unknowns are loud.** A function the BIOS does not have sets CF, puts an error code in AH and is reported through the
  machine's event hook (`PC98_EV_BIOS_UNKNOWN`); a jump into the page where no entry point is lands on `INT3`, is reported
  (`PC98_NOTE_ROM_ENTRY`) and stops the CPU (`PC98_STOP_ROM_ENTRY`). The trace is the to-do list.

The machine gained an event hook for tools (`Pc98.event`: every port access, unhandled ports, guest `INT n`, hardware
interrupts taken, BIOS calls before and after, unknown calls, the first read of each work-area byte, BIOS notes), a
`yield` flag that makes `pc98_run()` return early, and `bios.kb_polls` (below). `pc98_boot()`, the bare hand-over of the
first sector without a BIOS, is unchanged and still used by the synthetic demos of `test_pc98.py`.

## The boot hand-over, as the real IPL needs it

Observed with the owner's TH01 image (the trace, then a disassembly of the 512-byte IPL):

| What | Value | Evidence |
|---|---|---|
| where the first sector goes | 1FC0:0000, the whole sector (512 bytes on these disks, 1024 on a 2HD floppy) | the IPL computes its buffer as CS - 240h and calls code it loaded relative to it |
| DS | **0000h** | the IPL's first instruction reads the boot device from `[0584h]` through DS |
| `[0000:0584]` | the DA/UA of the boot device (80h: first SASI/IDE hard disk; 90h: first 1 MB floppy) | that same read; the boot menu and `IO.SYS` read it again |
| how it is entered | a **far call**: the IPL gives up with `RETF` (CX = 1) after its error path | the IPL's last instruction |
| AL | the DA/UA too (the placeholder convention; harmless, this IPL does not use it) | |
| SS:SP | 0000:7C00 minus the return address (our choice: below everything the IPL and menu touch, 1D800-1FFFF) | the IPL and menu only push and pop |
| interrupts | off | |

When the boot program returns, the BIOS reports `PC98_NOTE_BOOT_RETURNED` (with CX) and stops the CPU with
`PC98_STOP_BOOT_FAILED` (a real BIOS would try the next device).

## The boot chain, step by step (TH01, 286 at 10 MHz)

1. **IPL** (sector 0): INT 1Bh AH=8Eh, AH=84h (new sense: BX = 512), then reads sector 0 again, the partition table
   (sector 1) and 7 KiB of NEC's fixed-disk boot menu (sectors 2-15) to 1D80:0000 and far-calls 1E00:0000.
2. **The boot menu** checks `[0501h]` bit 3 (high-resolution machine: no), memory switch 5 (boot-device selection: the
   default order), `[055Dh]` (hard disks present), `[0460h]`-`[0478h]` and `[0482h]` (SCSI: none), AH=8Eh and AH=84h per
   drive, reads the partition table, finds the bootable DOS partition and reads its first sector (C1 H0 S0) to 1F80:0000,
   then jumps to it with `RETF`.
3. **The partition boot sector** sets SS:SP = 0000:028E and reads, with **relative sector numbers** (DA/UA 00h, DX:CX =
   sector), the root directory, 62 KiB of `IO.SYS` to 0060:0000 (BX = F800h) and 2 KiB more to 1000:0000, which it moves
   down to 0060:F800; then `JMP 0060:0000` (after 43,436 clocks).
4. **IO.SYS** fills vectors 20h-FFh with its own default, reads the work area (table below), the memory switches 1-7,
   hooks what it needs, sets the CRT mode (INT 18h AH=0Ah, 0Ch, 11h, 12h, 13h), initialises the keyboard (AH=03h), asks the
   calendar (INT 1Ch AH=00h), reads `MSDOS.SYS` and `COMMAND.COM` through INT 1Bh (CHS reads after AH=8Eh), looks for
   `CONFIG.SYS`, prints the banner and starts `COMMAND.COM`.
5. **COMMAND.COM** prints its version and runs `AUTOEXEC.BAT`. NEC's MS-DOS 3.30C does not know the `@` prefix, so TH01's
   first line is answered with an error and echo stays on: every command shows as `A>` and the command, the prompt line M3
   asked for. `MDRV98.COM` starts after 3.58 million clocks (`pc98run --until-exec mdrv98`).

The whole boot uses only these calls: INT 18h AH=03h, 0Ah, 0Ch, 11h, 12h, 13h; INT 1Bh AH=06h, 14h, 84h, 8Eh; INT 1Ch AH=00h.
No hardware interrupt is taken until `MDRV98`, the first program, sets timer counter 0 to about 10 kHz and unmasks IRQ 0
(it then takes some 1,900 timer interrupts while it looks for a sound board). Typed input works too: CTRL+C breaks
`AUTOEXEC.BAT`, `Y` confirms, `DIR /W` lists the disk and DOS waits at a new prompt (`test_dos_takes_keyboard_input`).

## INT 1Bh: disks

AL = DA/UA, AH = command in the low nibble (flags in the high nibble), BX = byte count (0 = 64 KiB), ES:BP = buffer. Status
in AH, CF set on error.

| DA/UA | Drive | Address |
|---|---|---|
| 80h-83h | hard disk unit 0-3 (an attached HDI) | CX = cylinder, DH = head, DL = sector **from 0**; a transfer runs on through the sectors, heads and cylinders |
| 00h-03h | the same units, **relative sector numbers** | DX:CX = sector number (DX high) |
| 90h-93h | 1 MB (2HD) floppy unit 0-3 (D88, FDI or raw image) | CL = cylinder, DH = head, DL = sector **from 1**, CH = size code; runs on to the next sector, then the other head, then the next cylinder |
| anything else (SCSI A0h/20h, 640 KB 70h, 1.44 MB 30h) | none | AH = 60h, CF |

| Command | Answer |
|---|---|
| x1h verify | the sectors exist: 00h; else C0h |
| x3h initialise | 00h |
| x4h sense | 00h (a write-protected floppy: 10h with CF clear). **84h (hard disk) "new sense"**: also BX = sector size, CX = cylinders, DH = heads, DL = sectors per track. 14h, which the IPL and menu use to look for a SCSI device (BL = 84h), leaves BL alone |
| x5h write | the sectors are written into the attached image's bytes (the harness's copy: the file is never written); a drive attached without a writable copy: 70h |
| x6h read | the sectors into ES:BP |
| x7h recalibrate | 00h |
| 8Eh (hard disk) | 00h, nothing else. The IPL, the menu and `IO.SYS` call it once per drive before using a disk with 512-byte sectors and none looks at the answer; an image has one fixed sector size, so there is no mode to set |
| 0Fh (hard disk) | 00h, nothing else. The STOP-key handlers of `IO.SYS` (3.30C and 6.20) call it for every hard disk and ignore the answer; an HLE disk has no operation in flight to give up |
| anything else (format, read ID, 0Eh and 0Fh on a floppy, ...) | 40h, CF, reported |

Errors: no such unit 60h; a sector beyond the image C0h (and `PC98_NOTE_DISK_RANGE`); write protected 70h. The BIOS keeps
every register but AX. DMA boundaries are not checked (the copy is done by the BIOS).

## INT 18h: keyboard and CRT

| AH | Function | Answer |
|---|---|---|
| 00h | read a key, waiting | AX = scan code << 8 \| character, removed from the buffer; waits (HLT with IF set) while it is empty |
| 01h | sense | BH = 1 and AX = the next key (left in the buffer), or BH = 0 |
| 02h | shift state | AL = `[053Ah]`: bit 0 SHIFT, 1 CAPS, 2 KANA, 3 GRPH, 4 CTRL |
| 03h | keyboard initialisation | buffer emptied, key-down bitmap and shift state cleared |
| 04h | key group AL | AH = `[052Ah + AL]`: one bit per key of key numbers 8*AL to 8*AL+7 that is down |
| 05h | read without waiting | BH = 1 and AX = the key (removed), or BH = 0 |
| 0Ah / 0Bh | set / get CRT mode | stored (also at `[053Ch]`); bit 0 20 lines, 1 40 columns, 2 semigraphics, 3 KCG dot access. Not acted on: the text screen stays 80 x 25 |
| 0Ch / 0Dh | text display on / off | text GDC BCTRL 0Dh / 0Ch |
| 0Eh | one display area from DX | the start address to the text GDC's scroll parameter RAM (the renderer ignores it) |
| 10h | cursor blink: AL = 0 blinking, 1 steady | stored |
| 11h / 12h | cursor on / off | stored |
| 13h | cursor position DX (byte offset in text VRAM) | stored; text GDC CSRW with DX / 2 |
| 16h | fill text VRAM: DL character, DH attribute | all 4096 cells; the memory switches in the attribute RAM are kept |
| 17h / 18h | beeper on / off | 8255 port C bit 3 (37h <- 06h / 07h) |
| 40h / 41h | graphics display on / off | graphics GDC BCTRL 0Dh / 0Ch |
| 42h | graphics mode: CH bits 7-6 area (11 = all 400 lines), bit 5 monochrome, bit 4 page | stored in `bios.gfx_mode` (the frame is always 640 x 400 here) |
| 14h | read a font pattern: DX = JIS code (ANK: DH = 0), **BX:CX = buffer (BX segment, CX offset)** | two header bytes, **height then width**, in units of 8 dots (2, 1 for ANK and for JIS rows 29h-2Bh, 2, 2 for full-width), then one byte per row (half-width) or two, left then right (full-width). The glyphs come from the font given to `pc98gfx_set_font`. Added for TH01's text (2026-11-03) and corrected the same day against the game's own code (it used to be DS:BX and width first); the full-width row order matches what TH01 reads, unverified against a real BIOS. |
| 1Ah | define a user character: DX = 76xxh or 77xxh, BX:CX = header and pattern as AH=14h returns | stored in the display module's user-character RAM; other codes ignored |
| anything else (KCG 1Bh, drawing 43h+, ...) | CF, reported |

**The keyboard interrupt (INT 09h, IRQ 1)** reads the 8251 (port 41h), keeps the key-down bitmap `[052Ah]`-`[0539h]` and
the shift state, sends EOI, and for a key press puts scan code << 8 | character into the 16-word ring at `[0502h]` (head
`[0524h]`, tail `[0526h]`, count `[0528h]`; when it is full the key is dropped and `[0529h]` counts it). The character
comes from the PC-98 keyboard layout (key numbers 00h ESC, 01h-0Dh the digit row, 0Eh BS, 0Fh TAB, 10h-1Bh Q to [, 1Ch
Return, 1Dh-28h A to ], 29h-33h Z to _, 34h space, 39h DEL, 3Ah-3Dh cursor keys, 40h-50h the keypad) with SHIFT, CTRL
(00h-1Fh) and CAPS; keys without a character (function keys, XFER, NFER, HELP...) give character 00h. **STOP** (60h) raises
INT 06h, which `IO.SYS` uses to break. Not translated: KANA (katakana) and GRPH (graphic characters); COPY's INT 05h is not
raised. **The DOS console driver reads this ring directly** (the count byte, then the head entry) rather than calling
INT 18h, which is why the exact layout matters; the machine counts guest reads of the count byte in `bios.kb_polls`
(cleared when a key arrives), which tools use to see that DOS is idle at a prompt.

## INT 1Ch: calendar and interval timer

| AH | Function | Answer |
|---|---|---|
| 00h | read the calendar to ES:BX | 6 bytes: year (BCD 00-99), month << 4 \| weekday (month 1-12 as a binary nibble, weekday 0 = Sunday), day, hour, minute, second (BCD) |
| 01h | set it from ES:BX | the weekday is recomputed from the date |
| 02h | interval timer: CX ticks of 10 ms, then ES:BX | ES:BX goes into INT 07h's vector; timer counter 0 is programmed for 10 ms (mode 2) and IRQ 0 unmasked; after CX ticks IRQ 0 is masked again and INT 07h is raised (the routine ends with IRET) |
| anything else | CF, reported |

The clock is the date the host sets (`pc98_bios_set_clock`, `pc98run --clock`; 2026-11-02 12:00:00 by default) advanced by the
machine's own time (CPU cycles / clock rate): runs are reproducible. Years are read as 2000-2099 for the weekday.

## Other vectors

| Vector | What the BIOS does |
|---|---|
| 00h-07h (CPU exceptions, STOP, COPY, timer routine) | `PC98_NOTE_EXCEPTION` and IRET. The same fault at the same place 1000 times in a row (a 286 fault returns to the faulting instruction) stops the CPU: `PC98_STOP_FAULT_LOOP` |
| 08h (IRQ 0) | the interval timer's tick (above), EOI |
| 09h (IRQ 1) | the keyboard (above) |
| 0Ah-0Fh, 10h-17h (IRQ 2-7, 8-15) | EOI (slave and master for 8-15) |
| 19h (RS-232C), 1Ah (printer), 1Fh (extended memory) | not present: CF, reported. **INT 1Ah AH=04h** is called twice by TH01's Anniversary Edition (`mov ah,4; xor cx,cx; int 1Ah`, CH then compared with 19h and 20h: a date or century read of the calendar chip); "not present" makes it choose its pre-2000 date path and that is what runs today. A reply with only CX = 2026h makes the title never brighten (the game then reads more of the reply), so the contract (what DX and the rest hold) is unknown (2026-11-03) |
| everything else before DOS replaces it | CF, reported |
| FD80:27EC (the INT 1Eh entry `IO.SYS` writes into the vector itself) and FFFF0 (reset) | a reboot: `PC98_NOTE_RESET`, the CPU stops (`PC98_STOP_RESET`); a warm boot is not modelled |

## The work area (0000:0400-05FF) and the memory switches

Everything not listed is 00h. "Read by" is what the trace (`pc98run --trace-work`) showed during the TH01 boot.

| Address | Value | Meaning | Read by |
|---|---|---|---|
| 0401h | 00h | extended memory in 128 KiB units: none | IO.SYS |
| 0458h | 00h | the boot menu tries more hard-disk units when bit 7 is set; IO.SYS's machine-type code reads bits 6 and 7 | menu, IO.SYS |
| 045Bh | 00h | (meaning unknown) | MDRV98 |
| 0460h-047Fh | 00h | SCSI drive parameters, 4 bytes per ID: none | menu |
| 0482h | 00h | SCSI units present: none | menu, IO.SYS |
| 0494h, 04BCh | 00h | (IO.SYS reads them while it counts drives; meaning not established) | IO.SYS |
| 0500h | 00h | BIOS flag 0. IO.SYS sets bit 6 itself on machines without the newer BIOS | IO.SYS |
| 0501h | 24h (A4h on an 8 MHz-family clock) | bits 0-2 = 4: 640 KiB (IO.SYS takes (n + 1) x 128 KiB); bit 3 = 0: normal resolution; bits 4-5 = 10b: a model with the newer BIOS (IO.SYS installs its own disk and INT 1Eh fixes when bits 4-6 are all 0); bit 7: the 1.9968 MHz timer clock of 8 MHz machines | menu, IO.SYS |
| 0502h-0529h | buffer, pointers 0502h, count 0 | the keyboard ring (above) | IO.SYS's console driver |
| 052Ah-053Ah | 0 | key-down bitmap, shift state | INT 18h AH=04h/02h |
| 053Bh | 0Fh | rasters per text row minus 1 (16-raster rows); TH01's CSRFORM routine ORs 80h into it and sends it to the text GDC (with 0 the text GDC drew rows one raster high). Not changed by AH=0Ah (the 20-line text mode is not modelled) | the games (TH01: 2930:00D3) |
| 053Ch | the CRT mode | stored by INT 18h AH=0Ah | the games (1C13:7380) |
| 054Ch, 054Dh | 00h | read once by ZUNSOFT and `ANNIV.EXE` (1C03:08F9, 1C13:7375); meaning not established, nothing visibly depends on the zero so far | the games |
| 055Ch | 00h | 1 MB floppies present (bits 0-3) | IO.SYS |
| 055Dh | 01h | bits 0-3: hard disks present (one per attached unit); bits 4-7: 640 KB floppies | menu, IO.SYS |
| 0564h, 056Ch, 0574h, 057Ch | 00h | per-floppy-unit bytes IO.SYS ORs C0h into | IO.SYS |
| 0584h | the DA/UA | the boot device | IPL, menu, PBR, IO.SYS |
| 05D8h-05DEh | 00h | IO.SYS ORs C0h into them (floppy driver state) | IO.SYS |

**Memory switches** 1-8 in the text attribute RAM at A3FE2, A3FE6, ... A3FFE: 48h 05h 04h 00h 01h 00h 00h 6Eh, the
factory-standard values. Switch 5's upper nibble (0: the standard boot order) is what the boot menu reads; IO.SYS copies
switches 1-7.

**Devices as the BIOS leaves them**: master 8259A ICW 11h/08h/80h/1Dh (vectors 08h-0Fh, the slave on IR7), IMR 7Dh (the
keyboard and the slave open); slave 11h/10h/07h/09h (vectors 10h-17h), IMR FFh; timer counter 1 in mode 3 at about 2 kHz
(the beep), the beeper off; the keyboard 8251 given a mode word and RxE; the text GDC on with the cursor at 0; the graphics
GDC off; the text screen spaces in white.

**Update 2026-11-03:** with the 386 model and the display module wired in, TH01 reaches its logo and title; the other images stop at their
write-protect question when run `--readonly`, TH04 at EMM386's request for protected mode. See [PC98.md](PC98.md), "The games on the 386
model". The table below is the earlier 286 survey and is kept for the record.

## Status of the owner's images (2026-11-02, 286 at 10 MHz, `pc98run`)

| Image | DOS | Where it gets to |
|---|---|---|
| th01 | NEC MS-DOS 3.30C | M3: banner, prompt lines, `MDRV98` (finds no sound board and installs), `ZUNSOFT` (draws its logo through the GRCG, which the graphics module will provide), then `ANNIV.EXE` stops on a **386 instruction**: `66 C7 06 ...` (`mov dword [516Fh], 0`), a fault loop through IO.SYS's INT 06h (the STOP-key handler) |
| th01e | 3.30C | `GAME.BAT`: `MDRV98`, `ZUNSOFT`, then `OP.EXE` asks INT 1Ah AH=04h twice (unknown: CF) and executes `66 26 0F BF ...` (`movsx eax, word es:[...]`, **386**) |
| th02, th02e | 3.30C | its `GAME.BAT` runs `ZUN.COM`, `MMD.COM`, `PMD.COM` (they probe the sound board, touch the GRCG and EGC ports, and `ZUN.COM` writes to the disk: with `--readonly` DOS stops at its write-protect question), then `OP.EXE` executes `66 C7 06 ...` (`mov dword [1B3Ch], 0`, **386**) |
| th03, th03e | 3.30C | `ZUN.COM -4` (`ZUNSP`) executes `66 0F B6 06 42 0F` (`movzx eax, byte [0F42h]`, **386**): the 286's INT 06h lands in `zuninit`'s STOP handler, which waits for the STOP key |
| th05, th05e | 3.30C | `OP.EXE` executes `66 FF 76 06` (`push dword [bp+6]`, **386**) |
| th04, th04e | NEC MS-DOS 6.20 | `HIMEM.SYS` loads and reports no extended memory; `EMM386.EXE` executes `0F 85` (a near `jnz`, **386**), the fault lands in DOS 6.2's INT 06h handler (a loop: the 286 returns to the faulting instruction). Needs the 386 core, extended memory and V86 or the plan's HLE XMS/EMS route |

So the BIOS is no longer the blocker: every game's own program needs the 386 real-mode instructions (operand-size prefix
66h, 0Fh opcodes), then graphics (GRCG at 7Ch/7Eh, EGC at 4A0h-4AFh) and sound (the drivers probe 088h-38Ah and A460h).
Ports E0D0h and E0D2h are read once by TH02's programs (unknown here). Speed: 400 million clocks of TH01 (39.5 million
instructions) take 0.84 s of host time, about 47 times the real machine. The boot is the same on the 8086, V30 and 286
models at 8 and 10 MHz.

## The harness

`python tools/pc98run.py --disk IMAGE [options]` builds `tests/host/pc98run.c` (gcc -O2, in WSL on Windows) and runs it; the
options are listed in the header of `pc98run.c`. The useful ones:

```
python tools/pc98run.py --disk "assets/local/(TH01-05)/disks/th01.hdi" --readonly --until-exec mdrv98 --dump-text --png build/pc98/th01.png
python tools/pc98run.py --disk IMAGE --trace-int=1b,18,1c --trace-work --trace-dos > build/pc98/trace.txt   # the to-do list
python tools/pc98run.py --disk IMAGE --keys "^cy\rdir /w\r" --keys-at 3 --until-key-wait --dump-text      # type at DOS
python tools/pc98run.py --disk IMAGE --trace-faults --cycles 400                                           # CPU gaps
```

`--break SEG:OFF`, `--history N`, `--dump ADDR:LEN`, `--state` help with the rest. The text dump shows `?` for kana and
kanji; the PNG uses the repository's 8x16 font (kanji blank, kana as boxes) or, with `--font`, the 8x16 ANK patterns at
offset 800h of a font ROM image the owner supplies (local use only: the font is the owner's, never in the tree).

## Tests

`python tools/hosttest.py discover -s tests/host -p "test_pc98bios.py" -v` (20 tests: the 17 below plus two font-call tests with a synthetic FONT.ROM and TH01 reaching its title on the 386 model). Synthetic, written for the tests
(`tests/host/pc98boot/bios*.asm` with nasm, three programs assembled by hand that run without it, images built in the
test): the hand-over (DS, CS, SP, `[0584h]`, the return), INT 1Bh on a hard disk (geometry, CHS across heads and
cylinders, relative sectors, write and read back, write protect, three errors, registers kept) and on a 2HD floppy
(multi-track, the whole 1024-byte boot sector), the keyboard (six keys with SHIFT and CTRL, shift state, key groups, an
empty buffer, a full one), the CRT calls, the calendar (set, read, the weekday, the year rolling over with machine time)
and the interval timer, the work area and switches, an unknown call, a divide error on the V30 and the 286 fault loop, an
unknown ROM entry and the INT 1Eh entry. With the owner's images present (`assets/local`, or `TOUHOU_PC98_DISKS`; the
tests skip otherwise): M2, M3 with a PNG, typed input at the prompt, TH02/TH03/TH05 reaching their batch files.

## Unverified, approximated or missing

- Everything above is from public PC-98 programming references and the trace; nothing is checked against real hardware.
  Unverified in particular: the meaning of `[0501h]` bits 4-6 and `[0500h]` (the values were chosen so that `IO.SYS` takes
  the newer-BIOS path), what AH=8Eh and 0Fh do on a real BIOS, the exact sense statuses, the stack the real BIOS boots with, the
  interval timer's use of INT 07h, the full-buffer behaviour (a real BIOS may beep), and the keyboard's character codes
  for the cursor and editing keys.
- Not implemented: floppy format, read ID and the other uPD765 commands, DMA boundary errors, 640 KB and 1.44 MB drives,
  SCSI, RS-232C and printer BIOS, INT 1Fh (extended memory), the font-pattern and user-character calls (no font ROM), KANA
  and GRPH translation, COPY's INT 05h, the CRT mode's 20-line and 40-column layouts, a warm boot, extended memory.
- The kanji in the text screen need a font (the graphics module's job); the harness renders ANK only.

## Next steps

1. The 386 real-mode instructions in the CPU core (the `agent/x86-386` branch): with them TH01's `ANNIV`, TH03's `ZUNSP` and
   TH05's `OP.EXE` get past the first instruction above. Re-run with `--trace-faults`.
2. The GRCG/EGC and the text font (graphics module) to see ZUNSOFT's logo and the title screens; then sound for `MDRV98`
   (it looks for a board at 0188h-038Ah and finds none).
3. TH04: extended memory for `HIMEM.SYS` (`[0401h]`, INT 1Fh, the 286's 16 MiB address space) and an answer for EMM386
   (the 386 core with V86, or the plan's virtual `CONFIG.SYS`).
