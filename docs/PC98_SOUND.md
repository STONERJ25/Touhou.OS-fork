# PC-98 FM sound: the YM2203 (OPN) and YM2608 (OPNA) model

A software model of the sound chips on NEC's FM boards, for the "Hakurei emulator" ([PC98.md](PC98.md)): the
PC-9801-26K (a YM2203 at 3.9936 MHz) and the PC-9801-86 (a YM2608 at 7.9872 MHz). The goal is that the Touhou games'
music drivers (`PMD`, `PMDB2`, `MDRV98`) can find the board, keep tempo with its timers and interrupts, and sound like
music. Branch `agent/pc98-snd`.

| File | What |
|------|------|
| `user/lib/pc98/include/opna.h`, `user/lib/pc98/src/opna.c` | the chip, the board decoding, the output FIFO and a resampler (part of `libpc98.a`) |
| `tests/host/opnatool.c` | host driver: register access, rendering, tables, a whole PC-98 around the board, WAV rendering of register scripts |
| `tests/host/test_opna.py`, `tests/host/opnamodel.py` | 55 host tests; an independent Python model of the FM, SSG, rhythm and ADPCM parts |
| `tests/host/opna/*.txt` | three register scripts for listening (FM scale, SSG chords, rhythm pattern) |
| `tests/host/opna/fmboard.asm` | a boot program that finds the board and lives on its timer interrupts, the way a driver does |

Like the rest of libpc98 it is freestanding (`<stdint.h>`, `<stddef.h>`, `memset`, `memcpy`), allocates nothing and
builds warning-free for the OS (`python tools/build.py`: 19 KB of code). `sizeof(Opna)` is 281,768 bytes, most of it
the -86 board's 256 KiB ADPCM RAM: make it static or allocate it. Every table (log-sine, exponent, SSG and rhythm
levels, vibrato ratios, the resampler's filter) is computed by `opna_init` from its definition with our own series for
sin, log2 and 2^x; floating point is used there only, the per-sample path is integer. Speed: 60 s of audio with all six
FM channels, the LFO and three SSG tones take 0.8-1.2 s of CPU on the owner's PC (gcc -O2 in WSL, depending on the
load of the moment): 50-70 times real time.

The rule of [PC98.md](PC98.md) holds here too: no ROM, no game data. The YM2608's six rhythm sounds live in its ROM, so
the host supplies them (`opna_set_rhythm_sample`); the tests use synthetic drums, and a local-only test renders with the
owner's copies from `assets/local` when they exist (the output stays in the git-ignored `build/`).

## What is modelled

| | YM2203 (-26K) | YM2608 (-86) |
|---|---|---|
| Ports | address/data, status 0, SSG and ID reads | both banks; status 1 with the ADPCM flags |
| Busy flag | 83 phiM after a data write (FM, control) | same, also after rhythm and ADPCM writes |
| Prescaler (2Dh-2Fh, by address write) | 1/6, 1/3, 1/2: FM 72/36/24 phiM per sample, SSG /4, /2, /1 | 144/72/48 phiM, SSG /8, /4, /2 |
| Timers A and B, flags, load/enable/reset, IRQ | yes | yes, plus 29h IRQ enables and 110h flag masks / IRQ reset |
| SSG: 3 tones, noise, 32-step envelope, mixer, I/O ports | yes | yes |
| FM channels | 3 | 6 (29h bit 7), stereo per channel (B4h-B6h) |
| 4 operators, 8 algorithms, feedback, DT/MUL, TL, KS, AR/DR/SR/SL/RR | yes | yes |
| LFO: AM (AMS, per-operator AM bit) and PM (PMS) | (none on this chip) | yes |
| Channel 3 special mode, CSM | yes | yes |
| SSG-type envelope (90h-9Eh) | approximate | approximate |
| Rhythm (six drums, 10h-1Dh) | (none) | host-supplied samples |
| ADPCM-B: CPU memory write/read with BRDY, playback, repeat, EOS, PCMBSY | (none) | yes, 256 KiB RAM |
| Board decoding | 188h/18Ah | 188h-18Eh, A460h (OPNA mode on/off, ID), A466h-A46Ch (the PCM FIFO's registers: see below, nothing is played) |

Not modelled: the ADPCM-B's A/D conversion and CPU-driven D/A playback (106h/107h prescale, 10Eh DAC, 10Fh PCM read
return 0), the ZERO flag (never set), ROM-attached ADPCM (101h bit 0), the test register (21h), the YM2203's and YM2608's
floating-point DAC (YM3014/YM3016: the output is the chip's integer sum, without the DAC's 10-bit mantissa rounding),
the -86 board's separate 16-bit PCM FIFO (ports A466h-A46Eh, used by `PMD86`: a different device, see below; **measured 2026-11-03: with `--opna 86` TH04 picks `PMD86.COM`, which waits on A468h forever, so a model or at least a settling stub of these ports is the next sound step, [PC98.md](PC98.md)**), the
PC-9801-73 and later boards, and the joystick (port A reads "nothing pressed" unless the host sets it).

## Interface (`opna.h`)

```c
static Opna fm;
opna_board_init(&fm, OPNA_BOARD_86, 12);    // a YM2608 at 7.9872 MHz on 188h-18Eh and A460h, the jumper on INT5 (IRQ 12)
opna_set_host_clock(&fm, cpu_hz);            // opna_advance() counts CPU cycles from now on
opna_set_irq_callback(&fm, irq_line, ctx);   // called with 1 / 0 whenever the board's interrupt line changes
int v = opna_board_in(&fm, port);            // -1: not the board's port
opna_board_out(&fm, port, value);            // -1: not the board's port
int line = opna_advance(&fm, cycles);        // let time pass; returns the line
size_t n = opna_take(&fm, frames, max);      // stereo frames at 55466.67 Hz that the elapsed time produced
```

Below the board: `opna_init(o, OPNA_YM2203 | OPNA_YM2608, phiM)`, `opna_write_addr/opna_write_data(o, bank, v)`,
`opna_read_status/opna_read_data(o, bank)`, `opna_reset`, `opna_ticks_to_irq` (host ticks until a timer raises the line,
for scheduling), `opna_set_port_input` (what the SSG's I/O pins read), `opna_set_rhythm_sample(o, i, pcm, count, rate)`,
`opna_set_mix(o, fm, ssg, rhythm, adpcm)` (256 = 1.0), `opna_render(o, frames, n)` (exactly n frames, advancing the chip
itself: for players without a CPU), `opna_rate_num/den` (the native rate as a fraction: phiM / sample clocks), and the
resampler (`OpnaResampler`, `opna_resampler_init(r, num, den, out_rate)`, `opna_resample`, `opna_render_resampled`).

Time: the chip's own clock is phiM; every FM sample period (72 phiM on the YM2203, 144 on the YM2608 at the default
prescaler, 55466.67 Hz on both PC-98 boards) the timers count, the envelope and phase generators step and, with
synthesis on (`o->synth`, the default), one frame goes into a 4096-frame FIFO (74 ms; when nobody drains it the oldest
frames are dropped and counted in `fifo_dropped`). With `o->synth = 0` only the timers, flags and ADPCM playback
position advance: a machine without audio output pays almost nothing.

## The register map this model relies on

Bank 0 (address 188h, data 18Ah on both boards), bank 1 (18Ch / 18Eh, YM2608 only).

| Register | Bits |
|---|---|
| 00h-05h | SSG tone period A, B, C: fine (8 bits), coarse (4 bits); TP 0 acts as 1 |
| 06h | SSG noise period (5 bits) |
| 07h | SSG mixer: bits 0-2 tone off A-C, 3-5 noise off A-C, 6 port A output, 7 port B output |
| 08h-0Ah | SSG level A-C: bit 4 envelope, bits 3-0 level |
| 0Bh-0Ch | SSG envelope period (16 bits) |
| 0Dh | SSG envelope shape: CONT, ATT, ALT, HOLD (a write restarts the envelope) |
| 0Eh-0Fh | SSG I/O ports A, B (on the PC-98 boards: A = interrupt jumper in bits 7-6, joystick in 5-0) |
| 10h | rhythm: bit 7 = 1 dump (stop) / 0 key on; bits 0-5 BD, SD, TOP, HH, TOM, RIM (YM2608) |
| 11h | rhythm total level (6 bits, 0.75 dB steps) |
| 18h-1Dh | rhythm BD..RIM: bit 7 L, bit 6 R, bits 4-0 level (0.75 dB steps) |
| 22h | LFO: bit 3 on, bits 2-0 frequency (3.98, 5.56, 6.02, 6.37, 6.88, 9.63, 48.1, 72.2 Hz) (YM2608) |
| 24h, 25h | timer A value NA: bits 9-2, bits 1-0 |
| 26h | timer B value NB |
| 27h | bits 7-6 channel 3 mode (00 normal, 01 special, 10 CSM, 11 special), 5 reset B, 4 reset A, 3 enable B, 2 enable A, 1 load B, 0 load A |
| 28h | key on: bits 7-4 S4, S3, S2, S1; bits 2-0 channel (0-2, 4-6) |
| 29h | YM2608: bit 7 six channels (SCH), bits 4-0 IRQ enable (ZERO, BRDY, EOS, timer B, timer A); 1Fh after reset |
| 2Dh, 2Eh, 2Fh | prescaler, chosen by writing the address alone |
| 30h-3Fh | DT (bits 6-4), MUL (bits 3-0); register offsets +0, +4, +8, +Ch are S1, S3, S2, S4 of channels 0-2 (+0/1/2) |
| 40h-4Fh | TL (7 bits, 0.75 dB) |
| 50h-5Fh | KS (bits 7-6), AR (bits 4-0) |
| 60h-6Fh | AM on (bit 7, YM2608), DR (bits 4-0) |
| 70h-7Fh | SR (bits 4-0) |
| 80h-8Fh | SL (bits 7-4, 3 dB steps, 15 = 93 dB), RR (bits 3-0) |
| 90h-9Fh | SSG-type envelope: bit 3 on, 2 attack, 1 alternate, 0 hold |
| A0h-A2h / A4h-A6h | F-number low 8 bits / block (bits 5-3) and F-number high 3 bits; the high byte is latched until the low one is written |
| A8h-AAh / ACh-AEh | channel 3 special mode: S3, S1, S2 (A2h/A6h is S4); separate latch |
| B0h-B2h | feedback (bits 5-3), algorithm (bits 2-0) |
| B4h-B6h | YM2608: L (bit 7), R (bit 6), AMS (bits 5-4), PMS (bits 2-0); C0h after reset |
| FFh (read) | YM2608 ID: 01h (the YM2203 reads 00h here, unverified) |
| bank 1 00h | ADPCM control 1: START 80h, REC 40h, MEMDATA 20h, REPEAT 10h, SPOFF 08h, RESET 01h |
| bank 1 01h | ADPCM control 2: L 80h, R 40h, RAM type 02h (1 = 8 bits wide: addresses count 32 bytes; 0 = 1 bit wide: 4 bytes) |
| bank 1 02h-05h | start, stop address (16 bits each, in those units; the stop unit is the last one played) |
| bank 1 08h | ADPCM data: CPU memory writes and reads |
| bank 1 09h-0Ah | DELTA-N: decoded samples per FM sample x 65536 |
| bank 1 0Bh | ADPCM level (x / 256) |
| bank 1 0Ch-0Dh | limit address: the pointer wraps to 0 there |
| bank 1 10h | flag control: bit 7 IRQ RESET (clears every flag, the mask stays), else bits 4-0 mask flags (1 = never set) |
| bank 1 30h-B6h | FM channels 3-5, as bank 0's |

Status 0 (read 188h): BUSY (bit 7), FLAG B (1), FLAG A (0). Status 1 (read 18Ch): BUSY, PCMBSY (5), ZERO (4), BRDY (3),
EOS (2), FLAG B, FLAG A. On the YM2608 both are masked by 110h.

## How it computes (and the choices behind it)

**Timers.** Timer A counts FM samples: an overflow every 1024 - NA samples (18 us x (1024 - NA) on both boards);
timer B every 16 samples: (256 - NB) x 16. Writing a load bit that was 0 reloads and starts the counter, writing it
again while 1 does not restart it; a new NA/NB is taken at the next overflow; a flag is set at an overflow only with
its enable bit; the reset bits clear the flags. The interrupt line is the OR of the flags (YM2203), or of the flags
that 29h enables and 110h does not mask (YM2608). Timer B's 16-sample prescaler starts at the load (unverified: on the
chip it may run freely, which would make the first period up to 15 samples shorter).

**SSG.** The tone counters step once per 8 SSG clocks and toggle their square at TP: f = phiM / (64 TP) on the YM2203,
phiM / (128 TP) on the YM2608 (the same pitch on both boards). Noise: a 17-bit shift register (taps 0 and 3) clocked at
half the tone rate, f = phiSSG / (16 NP). Envelope: 32 steps of 1.5 dB, one per EP ticks, so a ramp takes 256 x EP SSG
clocks; shapes 0-15 as on the AY-3-8910 (one ramp then silence, repeat, alternate, hold). Fixed levels map to 2v + 1
on the 32-step scale (0 = silent). A channel's output is its level when (tone or tone-off) and (noise or noise-off),
so both off gives a steady level, as sample-playing tricks expect. The SSG runs at its own rate (2.25 ticks per FM
sample) and is averaged over each sample (a box filter), then high-passed at about 9 Hz like the board's coupling
capacitor (`o->ssg_dc_block`). Registers read back as whole bytes (unverified for the unused bits).

**FM.** Per operator: a 20-bit phase accumulator advanced by ((F-number << block) >> 1) +/- detune (the manuals' DT1
table, 17-bit wrap-around), times MUL (0 = one half); the top 10 bits plus the modulation select a value of a 256-entry
quarter-wave log-sine table; the attenuation (envelope + 8 x TL + AM, 10 bits, 0.094 dB per step) is added (x 4) and an
exponent table turns it back into a linear 14-bit signed output (peak 8168). Modulation into an operator is half the sum
of its modulators' outputs; S1 feeds back the mean of its last two outputs scaled by FB (FB 7: 4 pi). Operators are
computed in the chip's slot order S1, S3, S2, S4, each using the latest output of its modulators, so S2 -> S3 (algorithms
0 and 2) sees the previous sample's S2 (unverified in detail). A channel is the sum of its carriers, limited to 14 bits
(unverified). Key code = block and two note bits from F11-F8 (the manuals' table); key scaling adds keycode >> (3 - KS)
to the rates.

The envelope generator steps every third sample; a global 12-bit counter decides for each rate (2 R + key scaling,
0-63) whether this step changes the attenuation: rates below 48 on every 2^(11 - rate/4)-th count by 0 or 1 following
four 8-step patterns, rates 48-59 on every count by 1-2, 2-4 or 4-8, rates 60-63 by 8. Attack: att += (~att x inc) / 16
until 0, then decay; attack rates 62 and 63 jump to 0 at key-on; decay to SL (3 dB steps), then sustain rate, release
rate 2 RR + 1. The fastest decay goes from 0 to 1023 in 128 steps = 384 samples (6.9 ms).

LFO (YM2608): a 7-bit counter advanced every 109, 78, 72, 68, 63, 45, 9 or 6 samples (these dividers reproduce the
manual's frequencies at 8 MHz to 0.3 %). AM: a triangle of 0-126 attenuation steps, shifted for AMS 0/1/2/3 to 0, 1.4,
5.9, 11.8 dB, on operators with the AM bit. PM: a triangle in 32 steps whose peak is the manual's deviation for PMS
(3.4 to 80 cents), applied as a ratio to F-number << block (the chip's own F-number-bit method is approximated).
Channel 3 special mode gives S1-S3 their own F-numbers; CSM keys all four operators of channel 3 on for one sample at
every timer A overflow (unverified: the length of the CSM key-on).

The SSG-type envelope is approximate: decay steps x 4, at attenuation 200h the shape repeats (attack again, at once for
attack rates 62-63), alternates the output's direction (turning where it is, without an attack), or holds (at full level
or silent), following the SSG's envelope shapes 8-15; the inversion is (200h - attenuation). The real chip's details
(what a slow attack does in this mode, the phase, the 200h-3FFh range) are not reproduced.

**Rhythm.** Each of the six drums plays its host-supplied PCM once from the start at key-on (linear interpolation from
the PCM's own rate), at (63 - total level) + (31 - level) steps of 0.75 dB, on the sides 18h-1Dh choose; a dump stops it.
The real chip plays 4-bit ADPCM-A at 18.5 kHz from its ROM: the sound depends entirely on the samples supplied.

**ADPCM-B.** The CPU memory procedures of the manual: RESET, the flag mask, IRQ RESET, control 1 = 60h (write) or 20h
(read), control 2 = 02h (8-bit RAM), limit, start, stop. Writing: the first data write loads the pointer from the start
register (the drivers write the addresses after the mode), each byte drops BRDY for 32 phiM, the byte at the stop
address sets EOS. Reading: two dummy reads load the pointer, then a byte per read. The pointer is the chip's own counter
over its whole address space (2 MiB with 8-bit RAM): stop and limit compare with it, and the board's 256 KiB answer to its
low 18 bits, so the RAM repeats (the drivers' RAM check writes the last 32 bytes with the stop address at FFFFh). BRDY in status 1 reads 1 whenever the
chip is ready, even after an IRQ RESET: the procedures reset the flags after every byte and wait for BRDY before the
next, and in the read procedure the reset comes after the read that started the next byte (unverified as a mechanism,
but it is what makes both procedures, and the drivers' RAM test, work; a latched BRDY flag still drives the interrupt
once per byte). Playback (control 1 = A0h, or B0h with REPEAT): DELTA-N / 65536 nibbles per FM sample through the
manual's decoder (X += (2 L + 1) / 8 x step, step x 0.9 ... 2.4 in 1/64: 57, 57, 57, 57, 77, 102, 128, 153, step in
127-24576; the chip's exact rounding is unverified), linearly interpolated, times the level / 256, to the sides 101h
chooses. PCMBSY while playing; EOS at the end (and at every lap with REPEAT).

**Output.** Each FIFO frame is (FM x gain_fm + SSG x gain_ssg + rhythm x gain_rhythm + ADPCM x gain_adpcm) / 256,
clamped to 16 bits. Defaults: FM 256 (the chip's own sum: one channel at full level peaks at 8168, six cannot all be at
full level without clipping, as on the chip), SSG 256 (a channel at level 15 = 8191 before the DC blocker), rhythm 128
and ADPCM 128 (their samples are 16-bit). The boards' analog FM/SSG balance is not documented anywhere we could use: these
defaults are a reasonable listening balance, not a measurement. The resampler is a 32-tap windowed sinc (Kaiser, beta 7)
in 64 phases with linear interpolation between them, cut off at 0.45 of the lower rate: a 440 Hz tone keeps its level
within 0.1 %, a 27 kHz tone (legal at 55.5 kHz, an alias at 48 kHz) comes out 51 dB down.

## A real driver on the board: TH01's MDRV98 and its title music (M6, 2026-11-03, branch `agent/pc98-m5m6`)

**Status:** way 2 below, `pc98_attach_opna(m, &fm, OPNA_BOARD_86, 12)` in `pc98mach.c`, now has tests (`FmBoardInTheMachine` in `tests/host/test_pc98.py`: the board's
clock is exactly `floor(cycles x 7987200 / cpu_hz)` after any run, a late attach counts from the attach, a status read in the middle of a 256-clock slice sees its own
time (the busy poll takes 4 reads, 9 without the sync: mutation-checked), timer B's interrupt reaches the slave PIC at exactly clock 147,693 for NB = E0h, the FIFO
holds one frame per FM sample) and has run the owner's real driver end to end. Run it yourself:

```
python tools/pc98run.py --disk "assets/local/(TH01-05)/disks/th01.hdi" --readonly --cpu 386 --mhz 16 --cycles 430 --quiet \
    --font "assets/local/(TH01-05)/DOSBox-X/FONT.ROM" --opna 86 --opna-rhythm "assets/local/(TH01-05)/DOSBox-X" \
    --opna-log build/pc98/th01_opna_log.csv --wav build/pc98/th01_start.wav --wav-rate 48000 \
    --type '250:\r' --type '300:\r' --type '330:i' --type '345:i'
python tools/wavstat.py build/pc98/th01_start.wav --window 1.0 --notes     # numbers for the WAV
python tools/opnalog.py build/pc98/th01_opna_log.csv --mhz 16               # the notes the driver programmed, per channel
```

(`--opna 86|26k` puts the board on the machine, `--opna-irq N` moves the jumper (default 12), `--opna-rhythm DIR` loads the six `2608_*.WAV` drum samples from the owner's pack, `--wav` writes the
board's output (resampled to `--wav-rate`, default 48000, 0 = the chip's 55,466 Hz), `--opna-log` the register writes with their CPU-clock time. All of it lands in `build/pc98/`, git-ignored.)

**What happened.** `MDRV98 -M7` (the first line of TH01's `AUTOEXEC.BAT`) finds the board at 188h at 3.2 million clocks and sets it up exactly as "How to check a real driver" below
predicts: the ADPCM RAM check (bank 1: 00h = 60h, the start and stop addresses FFFFh, four data bytes through 08h with the flag reset after each, readback), `29h = 83h`, the LFO off (`22h`), the timers
(`24h`-`27h = 3Fh` with NB = E0h), key-off of all six channels (`28h` 06h to 00h), the SSG silenced, `B4h`-`B6h = C0h` on both banks, the rhythm levels; 136 writes in the first 12 million clocks.
Its tempo interrupt (timer B, 32 x 16 FM samples = 9.23 ms = 147,692 clocks at 16 MHz) then arrives every **147,692 +/- 150 clocks** for the whole run (the handler writes `27h = 3Fh` each time; 2,950 of them
in the 27 s run). Nothing else on the way was unknown (the probes of 088h, 288h and 388h stay unhandled, as they should).

Over the whole run (logo, title, START: 26.9 s of machine time) the driver made **19,408 register writes**: SSG tone 4,036, level 530, mixer 25, noise 6; FM pitch 10,206, key 501, operator 880, channel 66;
rhythm 128; timer 2,951; ADPCM 72 (only its RAM check: the title music uses no ADPCM). The WAV (`build/pc98/th01_start.wav`, 1,290,001 frames at 48 kHz, 26.875 s: exactly the machine time) has: peak 19,550 of 32,767
(-4.5 dBFS, no clipping), RMS 2,383 (-22.8 dBFS), silence until **8.65 s** (the ZUNSOFT logo has no sound), then the title music until 21.02 s (START was pressed at 18.75 s: the driver fades the FM total levels and SSG
volumes one step per tick, then keys everything off), a noise burst of about 0.7 s at 24.5 s (SSG noise period 32h, level 0Fh decaying: a sound effect) and a short tone at 25.55 s. Four FM channels and two SSG channels carry the title
music, FM 1 and SSG 2 play the same melody (an arpeggio of 16th notes every 99 ms), FM 3 and 4 a slow bass of one note per 1.58 s, drums on the title only (hi-hat 16, snare 4, cymbal 4 key-ons).
**Every FM note lies within 1 cent of the equal-tempered scale (A = 440 Hz)** (the SSG sits 2 to 10 cents sharp, its period being an integer), and the spectrum agrees: the strongest components of the whole file are 293.8, 146.8, 220.0 and 440.1 Hz.
That is a check of the model's tuning against the driver's own note tables without any recording: a master clock or prescaler off by 0.1 % would show as a systematic offset. Tests: `test_th01_music_driver_finds_the_fm_board` and
`test_th01_title_music_plays_in_tune` in `tests/host/test_pc98bios.py` (they need the owner's images; they check behaviour, not the song).

**Not verified, nobody has listened:** whether it sounds right (the FM/SSG/rhythm balance is the board's analog mix, which is not documented), the SSG-type envelope and the 14-bit channel limit (the notes above come out musical, so
nothing is obviously wrong), whether the drum samples (`--opna-rhythm`) are at the right level, and every part of the chip that TH01's title does not use (ADPCM-B playback, the LFO: MDRV98 writes `22h = 00h`).
The stage's own music was not reached (see [PC98.md](PC98.md): the game quits to DOS after the stage-1 intro).
**Cost:** the process's CPU time for 21 s of machine time at 16 MHz is 1.97 s without the board and 2.47 s with it, synthesising, resampling to 48 kHz and writing the WAV: the board costs about 25 % of the machine's work here
(the wall clock of the same runs was three times longer on this PC while other jobs ran: use `CPUTIME` in the harness's output, not `SPEED`, to compare).

## Wiring it into the machine (for the owner of `pc98mach.c`)

**Status 2026-11-03:** way 2 below exists as `pc98_attach_opna(m, &fm, OPNA_BOARD_86, 12)` in `pc98mach.c` (the board's ports tried after
the display's, `opna_advance` at every `pc98_advance` and before each access to the board's ports, the interrupt through `pc98_irq`, the
machine's `m->fm` pointer). It is tested and has run the owner's real driver (section above); the harness option is `--opna`.

The model does not touch the machine; two ways in, both tested by `opnatool`'s machine mode:

1. **Through the existing host hooks** (no change to `pc98mach.c`, what `tests/host/opnatool.c` does):
   ```c
   static uint64_t fm_synced;
   static void fm_sync(Pc98 *m) { opna_advance(&fm, m->cpu.cycles - fm_synced); fm_synced = m->cpu.cycles; }
   static int fm_in(void *h, uint16_t port) { fm_sync(h); return opna_board_in(&fm, port); }
   static int fm_out(void *h, uint16_t port, uint8_t v) { fm_sync(h); return opna_board_out(&fm, port, v); }
   static void fm_irq(void *h, int level) { pc98_irq(h, fm.board_irq, level); }

   opna_board_init(&fm, OPNA_BOARD_86, 12);
   opna_set_host_clock(&fm, m->cpu_hz);
   opna_set_irq_callback(&fm, fm_irq, m);
   m->host = m; m->port_in = fm_in; m->port_out = fm_out; fm_synced = m->cpu.cycles;
   for (;;) {                                          // the frame loop
       uint64_t slice = 2000, t = opna_ticks_to_irq(&fm);
       if (t != UINT64_MAX && t + 1 < slice) slice = t + 1;      // the board's interrupt arrives on time
       pc98_run(m, slice);
       fm_sync(m);
       n = opna_take(&fm, frames, max);                // to the audio stream
   }
   ```
2. **Inside the machine** (cleaner later): an `Opna *fm` in `Pc98`, `opna_board_in/out` tried in `pc98_in8`/`pc98_out8`
   before the host hooks, `opna_advance(m->fm, cyc)` in `pc98_advance`, the callback on `pc98_irq`. `pc98_run`'s
   256-cycle slices then bound the interrupt latency to 25 us at 10 MHz without the scheduling above.

The ports: 188h/18Ah (both boards), 18Ch/18Eh (the -86's second bank: they read FFh and ignore writes until A460h bit 0
is set, which is how drivers tell an -86 from a -26K-compatible setting), A460h (-86: reads 40h | bits 1-0 as last
written; the ID nibble 4 is unverified). The interrupt: the board's jumper, factory setting INT5 = IRQ 12 (slave IR4,
vector 14h); INT0 = IRQ 3, INT41 = IRQ 10, INT6 = IRQ 13. `opna_board_init` puts the jumper into SSG port A bits 7-6
(00 INT0, 01 INT6, 10 INT41, 11 INT5), where the drivers read it. The line is a level: high while a flag the IRQ logic
sees is set; the PC-98's 8259s run edge-triggered, so drivers reset the flag in the handler and the next overflow is a
new edge. The -26K board's base can be moved by a jumper (088h, 188h, 288h, 388h): `o->board_base`.

## PMD86 on the board: TH02, TH04 and TH05 (M7, 2026-11-03)

With the PCM registers below the PMD driver of TH02 to TH05 runs on `--opna 86`, and its music comes out of the model the same way MDRV98's did (the numbers are from the harness's WAV and register log, `tools/wavstat.py` and `tools/opnalog.py`; the
runs are the ones of [PC98.md](PC98.md)'s survey with Z presses through the menus; nobody has listened):

| Game, run | Register writes | FM channels | FM pitch off the equal-tempered scale | SSG | Drums (rhythm key-ons) | WAV |
|---|---|---|---|---|---|---|
| TH04, 56 s (intro, title, weapon select, stage 1) | 26,660 | all six | at most 0.7 cent | channels 1 and 2 within 7 cents, channel 3 up to 35 (a sweep or vibrato; not looked into) | bass 65, hi-hat 45, cymbal 44, snare 44 | 56.25 s, peak -3.5 dBFS, RMS -21 dBFS, silent before 4.2 s |
| TH05, 44 s (intro, loading, stage 1) | 26,200 | all six | at most 0.7 cent | up to 35 cents | bass 45, hi-hat 137, snare 40, cymbal 6 | 43.75 s, peak -2.1 dBFS, silent before 1.8 s |
| TH02, 44 s (intro, title, stage 1) | 12,615 | all six | at most 10.5 cents (the driver's detune settings) | up to 35 cents | hi-hat 69, bass 66, snare 32, tom 21, cymbal 15 | 43.75 s, peak -0.2 dBFS (no clipped sample), silent before 14.6 s |

The drums are the owner's `2608_*.WAV` samples (`--opna-rhythm`). TH02's peak is the highest: the model's mix (FM 1.0, SSG 1.0, rhythm and ADPCM 0.5) is a listening balance, not a measurement of the board's analog stage, and a louder passage
than the ones recorded could clip. That all six FM channels are in use and tuned to the scale within a cent is a check of the six-channel switch, the second bank's registers and the key-on of channels 4-6 that MDRV98 (three channels in its title) did not give.

## The -86 board's PCM FIFO (A466h-A46Ch): registers only, from reading PMD86 (2026-11-03)

TH02 to TH05 (`ZUN.COM -o` finds A460h answering) start **PMD86.COM**, the driver for the -86 board, which also feeds the board's own 16-bit PCM FIFO. Until this stub existed every one of them hung there: PMD86's interrupt entry
(the vector of the board's IRQ, shared by the FM timers and the FIFO) reads **A468h**, and **while bit 4 is set it serves the FIFO and reads again**, with interrupts off, before it looks at the FM chip's timer flags; the
unhandled port read FFh, so the flag never cleared. What PMD86's code does with the ports (read in the extracted driver for understanding; nothing was copied, and the file stays in `build/`):

| Port | What PMD86 does | Model |
|---|---|---|
| A466h | write `A0h` or `(15 - level)`: the PCM volume | read back as written |
| A468h | control, changed one bit at a time with read, modify, write: bit 7 play, bit 5 interrupt enable, bit 4 interrupt flag (read: pending; the acknowledge is write 0 then 1), bit 3 FIFO reset (set, then cleared), bits 2-0 a rate select (`xor 7` of a 2-bit field) | reads back as written except **bit 4, which reads 0 (no interrupt is ever pending)** |
| A46Ah | `F2h` while resetting, `01h` when starting | read back as written |
| A46Ch | the FIFO's data port: bytes in pairs (`out dx,al` twice per loop turn), 100h pairs to prefill at the start and 80h pairs per interrupt | taken and counted (`pcm_bytes`), dropped |

Start: A468h bits 7, 6, 5, 4 cleared with a pulse of bit 3, A46Ah = F2h, a prefill of 100h pairs, A468h |= 10h, |= 20h, A46Ah = 01h, A468h |= 80h. Stop: bit 7 cleared, a pulse of bit 3, bits 5 and 4 cleared, bit 4 set.
**Not modelled, and what PCM parts of a song need:** the FIFO's size, its consumption at the rate A468h and A46Ah select (no table is known here), the moment the interrupt flag rises (probably when the FIFO has room for a chunk), the sample
format (A46Ah) and the mixing into the output. With the flag never raised PMD86's PCM engine stops after its first fill: the songs' PCM parts are silent, everything else (FM, SSG, timers, tempo) runs. Tests: `Pcm86` in `tests/host/test_opna.py`,
`test_pmd86_gets_past_the_pcm_fifo` in `tests/host/test_pc98bios.py` (TH05 and TH02 reach `OP.EXE` with `--opna 86`; mutation-checked).

## Into the OS's audio library

The chip's rate is 55466.67 Hz; `/dev/audio` takes 4000-192000 Hz and converts everything to 48 kHz with linear
interpolation ([AUDIO.md](AUDIO.md)). Either let the kernel convert (`audio_set_format(a, 55467, 2, AUDIO_FMT_S16)`,
0.0006 % fast, simplest), or resample in the program for better quality:

```c
Audio *a = audio_open(AUDIO_NONBLOCK);              // 48 kHz stereo 16-bit by default
OpnaResampler rs;
opna_resampler_init(&rs, opna_rate_num(&fm), opna_rate_den(&fm), 48000);
size_t n = opna_take(&fm, in, 1024), used = n;      // what the last frame of emulation produced
size_t m = opna_resample(&rs, in, &used, out, 1024); // all n are used unless out is full
audio_write(a, out, m * 4);
```

Keep about 100 ms queued (`audio_status`): the emulation produces sound at machine speed, the hardware plays at wall-clock
speed, and an underrun is silence, not a glitch of old sound.

## Tests

`python tools/hosttest.py discover -s tests/host -p "test_opna.py" -v` (WSL on the owner's PC; 55 tests, about 30 s).
nasm is needed for the machine test (skipped without it).

| Class | What |
|---|---|
| `Timers` | timer A and B overflow after the exact sample counts on both chips, reset, enable and load bits, a new value at the next reload, 29h enables and 110h masks / IRQ reset, BUSY for 83 clocks after data writes only, the host clock conversion and `opna_ticks_to_irq`, the prescaler by address writes, one interrupt edge per overflow |
| `Registers` | power-on state and size, SSG read-back, ID register, both boards' port decoding, the A460h second-bank switch, the interrupt jumper in port A |
| `Tables` | log-sine, exponent, SSG, rhythm and vibrato tables equal to their definitions computed by Python; operator lookups; phase increments for every DT, several MULs and vibrato, the detune wrap-around |
| `Ssg` | square waves against closed formulas on both chips (pitch, level), fixed levels, noise, all 16 envelope shapes, three channels mixed, the DC blocker |
| `Fm` | sample-exact agreement with `opnamodel.py` for every algorithm (random patches, feedback), six channels with notes and levels changing while playing, envelope traces through attack/decay/sustain/release with key scaling, the fastest decay against the counter, LFO AM and PM, the YM2203 ignoring the LFO, channel 3 special mode, CSM, pan, the six-channel switch, the SSG-type envelope's shapes (in dB: repeats jump, alternations turn, holds hold), release to silence, a CRC regression guard |
| `Rhythm` | one drum at three levels, pan, dump and re-key; six drums mixed; no samples, no sound |
| `Adpcm` | the memory write and read procedures with the BRDY handshake (and the flag reset between bytes), EOS at the stop address, the top of the RAM with the stop address beyond it (no EOS, the RAM repeats), playback at three DELTA-N values against the Python decoder, the codec tracking a sine, REPEAT with an EOS interrupt, SPOFF |
| `Resampler` | a tone keeps pitch and level at 48 and 44.1 kHz; a 27 kHz tone is removed |
| `Scripts` | the three scripts render; the scale's 15 pitches and the panned chord, the SSG chord tones and fade, the bass, drums from synthetic samples |
| `OwnerRhythmSamples` | (local only) renders `rhythm.txt` with the owner's six drum samples into `build/opna/rhythm_rom.wav` |
| `Machine` | `fmboard.asm` on the V30 core and the PC-98 skeleton with each board and four jumper settings: detection, the second bank, the jumper, eight timer B interrupts on time, the note heard |
| `Speed` | the optimised build renders 60 s of a busy six-channel patch in about a second |

The Python model was written from the same descriptions as the C (tables from Python's `math`, closed formulas for the
SSG, the manual's ~att form of the attack); it guards against slips in the C, not against a shared misreading of the
chip. Changing the feedback scaling or one envelope pattern in the C makes a dozen `Fm` tests fail.

**Listening.** `opnatool --script tests/host/opna/fm_scale.txt --out build/opna/fm_scale.wav --rate 48000` (the tool
is `build/host-x86-wsl/opnatool` after a test run; `--rate 0` keeps 55466 Hz; `--rhythm DIR` loads `2608_BD.WAV` ...
`2608_RIM.WAV` from DIR, 16-bit mono at any rate). The script format: `chip 2203|2608 [clock]`, `w BANK REG VAL` (hex),
`wait MS`, `at MS`, `mix FM SSG RHY ADPCM`, `end MS`, `#` comments. A test run leaves `fm_scale.wav`, `ssg_chords.wav`,
`rhythm.wav` (and `rhythm_rom.wav` with the owner's drums) in `build/opna/`.

## Unverified (against hardware or a manual we could read)

The busy duration (83 phiM, from the manuals' wait table, applied to every non-SSG data write); the YM2203's read of
register FFh (00h here) and of other non-SSG registers (00h); whole-byte SSG read-back; timer B's prescaler phase; the
interrupt enables at reset (1Fh); IRQ RESET clearing the timer flags too; channels 4-6 being silent and not keyable
with SCH = 0; one F-number latch for all channels; the operator order delays; the 14-bit channel limit; the vibrato's
exact steps; the CSM key-on length; the SSG-type envelope; the SSG's fixed-level mapping (2v + 1) and its 1.5 dB DAC
steps; the ADPCM-B step multipliers' rounding, its interpolation, the BRDY mechanism and timing (32 phiM per byte), the
address units for 1-bit RAM; A460h's ID nibble; the analog mix of FM, SSG, rhythm and ADPCM.

## How to check a real driver

The drivers on the owner's disks can be read for understanding (never committed: extracted files and disassemblies
stay in `build/`): `python tools/hdi-look.py "assets/local/(TH01-05)/disks/th05.hdi" --extract PMD.COM build/pc98-drivers/PMD.COM`,
then `ndisasm -b 16 -o 0x100` (nasm's disassembler) and look for the port numbers (`mov dx,0x188`, `0xa460`). What the
start-up of `PMD`/`PMDB2` (and the games' own board check, which works the same way) does, in our words:

1. **Find the chip.** For each possible base (088h, 188h, 288h, 388h) write an SSG register (envelope period, 0Bh) and
   read it back. `MDRV98` reads the ID register (FFh) first and falls back to the SSG mixer (07h) read-back.
2. **Tell an -86 from a -26K.** Read A460h (FFh: no -86 board); clear its bits 0-1; read the ID register through the
   first bank (01h = YM2608) and check that the second bank's ports now read FFh; then set A460h bit 0 for OPNA mode.
   On PC-9821 machines a ROM signature and a mute port are consulted as well (not this module's business: the machine
   answers FFh there, which is a PC-9801).
3. **Find the interrupt.** Write 07h (mixer, port A as input), read port A (0Eh), bits 7-6 -> INT0/INT6/INT41/INT5, and
   install the handler at that vector; compute the 8259 mask bit and specific EOI from it.
4. **Initialise.** 29h = 83h (six channels, timer interrupts), the noise period, B4h-B6h = C0h, LFO off (22h), the
   flag control 110h = 80h then 18h (flags reset, then BRDY and ZERO masked).
5. **ADPCM RAM (`PMDB2`, and the games' check that decides between `PMDB2` and `PMD86`).** Write 32 bytes at the top of
   the 256 KiB (start unit 1FFFh with 8-bit RAM) through the memory write procedure, polling BRDY with a time-out and
   resetting the flags after each byte, read them back after two dummy reads, compare. If this fails the games choose
   `PMD86`, which uses the board's separate PCM FIFO (A466h-A46Eh) that this module does not model: the machine should make
   the ADPCM test succeed (it does with this model) or also model the -86 PCM.
6. **Tempo.** Timer A at NA = 0 and timer B at the song's tempo, 27h = 3Fh (reset, enable and load both); the handler
   reads status 0 until neither flag is up, writing 27h with the reset bit of the timer it served each time.

Every register write waits for BUSY to clear before the address write, with a calibrated delay between address and
data. To watch a driver against this model: run it in the machine mode of `opnatool` (a DOS environment for the
driver is the BIOS agent's work) or log `opna_board_out` calls in the machine's port hook; the register sequence above
is what to expect.
