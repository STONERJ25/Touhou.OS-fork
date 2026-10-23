# Sound

The sound system belongs to the Kappa/Nitori hardware crowd (kernel log tag `Nitori`). Reimu's Jukebox is the music player,
staged in the Prismriver Concert Hall.

```
 Jukebox  beep  play  volume  panel speaker  desktop sounds     user/bin/jukebox/, user/bin/kappabox/audio.c, hakurei-shell
        |  libaudio  (device, WAV, generators, songs and the synthesizer, module player, system sounds)    user/lib/audio/
        v  /dev/audio (write PCM, ioctls) and /proc/audio
 audio core  kernel/drivers/audio/audio.c     streams, software mixer, volume, limiter, mixer thread
        |  AudioBackend: a looping 48 kHz 16-bit stereo DMA ring + "where is the hardware now?"
        v
 hda.c + hda_graph.c  Intel High Definition Audio (QEMU intel-hda; the Dell's 8086:a171 + Realtek codec)      ac97.c  AC'97
```

## Status (read this first)

| Piece | State |
|---|---|
| Audio core: `/dev/audio`, mixer, per-stream and master volume, mute, soft limiter, `/proc/audio`, underruns are silence, instant flush | done, tested in QEMU |
| Intel HDA driver: generic codec graph walk, output planner, auto-mute, immediate-command and position-buffer fallbacks | done, tested in QEMU (`hda-output`, `hda-duplex`, `hda-micro`, and a controller with no codec) and, for the planner, against invented Realtek-style codecs; **never run on real hardware** |
| AC'97 driver | done, tested in QEMU (`AC97`); never run on real hardware |
| libaudio: device, WAV reader/writer, tone and noise generators, system sounds | done |
| Song engine: pulse / triangle / saw / noise / 2-operator FM voices, the `.tms` pattern format, parser, sequencer | done, tested (pitch, tempo, bar alignment) |
| Five original songs (`/ScarletDevilMansion/share/music/*.tms`) | done; **nobody has listened to them**: the checks are measurements, not ears |
| ProTracker `.mod` player (4 to 32 channels, the usual effects, Amiga panning) | done, tested with generated modules (pitch, panning, seeking and twenty effects, each against a model); **never run on a real tracker tune**. `.s3m`, `.xm`, `.it` and 15-sample modules: **not done** |
| `beep`, `play` (WAV, raw, songs, modules), `volume` | done |
| Reimu's Jukebox (playlist, play/pause/stop/seek, volume, level display, `~/Music`) | done, GUI-tested |
| System sounds (login, dialog, error, shutdown) in the shell, greeter, lock screen, Kappa dialogs, switch `nosounds` / `HAKUREI_SOUNDS` / `/etc/hakurei/sounds` | done, tested |
| Volume control in the panel | done, tested |
| Recording (capture) | not done (the task called it optional) |
| Tests | `tests/case_audio.py`, `tests/host/hdatest.c`, `tests/guitext.py` (see Tests) |

## The device

`/dev/audio` (character device, group `shrine`, mode 0660) exists only when a sound controller with a usable output was found.
Without hardware, or with `noaudio` on the kernel command line, opening it fails with `ENOENT` and every tool says
"this machine has no sound device".

Each `open()` is a stream. Nothing is allocated in the hardware until the first `write()`; opening only to change the
master volume (`volume` does this) costs nothing. Per stream:

* a format: 8-bit unsigned, 16/24/32-bit signed little endian (24-bit is packed in three bytes), 1 or 2 channels,
  4000 to 192000 Hz. Default 48 kHz stereo 16-bit. `AUDIO_SET_FORMAT` fails with `EBUSY` while bytes are queued.
* a 256 KiB byte queue. `write()` blocks until everything fits (or returns what fits with `O_NONBLOCK`, `-EAGAIN` if
  nothing does); `poll()` reports `POLLOUT` when a quarter of the queue is free. Writes may be any number of bytes; the
  mixer assembles frames.
* a volume (0 to 100 per channel) and a mute flag.

`close()` never blocks and never cuts off sound already written: the stream stays until its queue has been played out.
`AUDIO_FLUSH` throws the queue away (and, when no other program has sound waiting, the 100 ms that are already mixed, so
pause, seek and stop are heard at once); `AUDIO_DRAIN` waits until everything has been heard.

ioctls are in `shared/audio_abi.h`: `AUDIO_SET_FORMAT`, `AUDIO_GET_FORMAT`, `AUDIO_SET_VOLUME`, `AUDIO_GET_VOLUME`,
`AUDIO_SET_MASTER`, `AUDIO_GET_MASTER`, `AUDIO_GET_INFO`, `AUDIO_GET_STATUS` (bytes queued, frames mixed, how far ahead of the
speakers the hardware ring is: what the Jukebox uses to know where the music is), `AUDIO_DRAIN`, `AUDIO_FLUSH`. No new
system calls are used (the numbers 100-105 stay free).

## The mixer

One kernel thread (`audio`, wakes every 5 ms while sound is playing, sleeps otherwise) keeps the hardware ring filled
about 100 ms ahead of the play position:

```
ring (65536 frames = 1.4 s):  [ played: zeroed ][ hw reads here ... mixed audio ... produced ][ zeros ]
```

The ring is long on purpose: the hardware keeps playing at wall-clock speed even when the (emulated) CPU is starved for a
second, and the only stale sound it could ever repeat is what is still in the ring when it laps it.

* For every stream it converts to 48 kHz stereo (linear interpolation, 16.16 fixed point), applies the stream volume and
  the master volume, and adds into a 32-bit accumulator. Everything is integer (the kernel has no SSE).
* A soft limiter (linear up to 28000 of 32767, then a rational knee that never reaches full scale) replaces hard
  clipping, so two loud programs do not wrap around into noise.
* The frames behind the hardware position are zeroed as soon as the hardware has passed them. If the thread is late, the
  hardware plays zeros, never old sound ("an underrun is silence"). A burst of sound after silence starts 20 ms ahead of
  the play position so its first samples are not lost.
* If the hardware position stops moving for 400 ms while sound is waiting (a controller that does not run), the core logs
  one line and discards sound at the right speed instead of letting programs hang. `/proc/audio` says "stalled".
* When nothing has played for 3 s (`audio.idle=MS` on the command line) the DMA engine is stopped and the ring cleared.
* The master volume (default 60) lives in the kernel and lasts until the next boot. The curve is quadratic: the gain is
  `(v/100)^2`, so 50 is a quarter of the amplitude of 100. The hardware amplifiers are set to 0 dB and left alone.

## Intel HDA

Two files. `kernel/drivers/audio/hda.c` talks to the hardware; `hda_graph.c` / `hda_graph.h` is pure logic that decides
from the codec's widget graph which pins to drive and how (it has no hardware or kernel calls, which is what lets
`hdatest` run it against made-up codecs, see Tests). Polled; there are no interrupts and no MSI.

**Controller.** PCI class 0x0403; memory space and bus master are enabled (`pci_enable`), and on Intel the traffic class
select (config 0x44) is cleared and the no-snoop bit (0x78 bit 11) turned off, as Linux does. The link is reset
(`GCTL.CRST` low, at most 100 ms, 2 ms pause, high, at most 200 ms, 2 ms) and `STATESTS` is polled for up to a second for
codecs to announce themselves; if none does, the reset is done once more. CORB and RIRB sizes come from the capability
bits (256, 16 or 2 entries), the base address registers get the upper 32 bits only if `GCAP.OK64` is set (all DMA memory
is below 4 GiB anyway), the read pointer reset handshake is done, and the response-count flag in `RIRBSTS` is acknowledged
after every command (QEMU stops reading commands until it is). If the rings do not answer, the first command is retried
through the immediate command registers (`ICOI/IRII/ICIS`) and those are used from then on. Three commands in a row without
an answer mark the codec dead: the driver logs one line and gives up, so a missing or unpowered codec (the HDMI codec on a
machine without a graphics driver, say) costs a fraction of a second and never hangs the boot. The stream position is read
from LPIB; the DMA position buffer is switched on as well and takes over if LPIB stands still while the buffer moves.

**Codec.** For each present codec the audio function group is taken to D0 (polling the actual power state) and every widget
is read into an `HdaGraph`: type and capabilities, connection list (short and long form, ranges), pin capabilities and
default configuration, amplifier capabilities and PCM rates/sizes (the function group's values where the widget has no
override of its own). `hda_plan_outputs()` then chooses:

* Output pins: output capable, not marked "no physical connection", and declared by the BIOS as line out, speaker or
  headphone. Of the line-out pins that share a default association (a 5.1 board's green, black and orange jacks) only
  the one with the lowest sequence number is driven; the others would repeat the stereo signal. If the BIOS declared no
  output at all, one pin of the device type "other" is tried, and `/proc/audio` says it is a guess.
* For each pin a path pin <- mixers/selectors <- converter by depth-first search (at most 10 widgets deep, cycles and
  dangling connections are skipped; pins are never stepping stones). The converter must be analog (not an S/PDIF/HDMI
  converter) and able to do 16-bit 48 kHz PCM; one nobody else uses is preferred, and a selector (or a pin with several
  sources) is never asked to pick two different inputs for two pins. Several pins may end up sharing a converter when the
  graph leaves no choice; every converter that carries the stream listens to the same stream tag.

`hda.c` then powers every widget on the paths (D0, polled), closes all inputs of every mixer on a path, selects the
connections (`SET_CONNECT_SEL` on selectors and multi-input pins) and opens exactly the mixer/selector inputs the paths
need (two pins may share a mixer: one path must not close the input the other uses), unmutes every output amplifier at its
0 dB step, and switches the pins on (`OUT_EN`, plus `HP_EN` on headphone-drive capable headphone pins, plus EAPD where the
pin has it). One output stream (tag 1, 48 kHz 16-bit stereo) loops over a 256 KiB ring described by 16 buffer descriptors;
the converter format is read back after it is set and a mismatch is logged.

**Jacks.** Pins that can sense a jack (presence detect, a jack or jack-and-fixed connection) are polled four times a second
while sound plays (pins that say "trigger required" are asked to sense first). `hda_plan_automute()`: headphones plugged in
switch the speaker and the line out off, a plugged line out switches the speaker off, pins that cannot sense count as
plugged, and if that would leave every pin off they are all switched on (a silent machine because jack sensing is broken is
worse than sound on an empty jack).

**Failure is quiet and loud enough to diagnose.** A codec that stops answering, has no analog output, or a controller that
will not reset gives one `[Nitori]` log line and no `/dev/audio`. `cat /proc/audio` prints the controller (streams, DMA width,
command path, position source), the codec ids, every chosen pin with its configuration word, jack state and path, and every
other pin with its configuration and capabilities; with `hda.dump` on the kernel command line every widget of the codec is
logged (`dmesg | grep 'hda:  0x'`). With that and `dmesg | grep Nitori` a silent jack on the real machine can be diagnosed
without a debugger. Two more command line words exercise the fallbacks on any machine: `hda.imm` (immediate commands instead
of CORB/RIRB) and `hda.posbuf` (position from the DMA position buffer).

**Not verified, not done.** Everything above ran only against QEMU's three codecs `hda-output`, `hda-duplex` and `hda-micro`
(one converter and one output pin each);
the Realtek-style trees in `hdatest` are invented, not recorded from a real machine, so they show that the planner copes
with those shapes (mixers that add converters and input pins, selectors in front of pins, bidirectional pins, a digital
converter, jack associations), not that it is right for the Dell's ALC3xxx. Never exercised on anything but QEMU: a
controller with fewer than 256 ring entries, LPIB vs. position buffer disagreement, a real multi-codec machine, jack sensing
and the auto-mute switch-over (QEMU's codecs have no jack sense), EAPD, headphone amp enable. Not implemented: vendor
specific coefficient programming (Realtek `COEF` tweaks some laptops and the headset-mode jacks need), GPIO controlled
external amplifiers, digital outputs (S/PDIF, HDMI), input streams (recording), powering down idle widgets, more than
two channels. The Intel PCI quirks above are from memory of Linux's `hda_intel.c`, not tested on a Kaby Lake PCH.

## AC'97

`kernel/drivers/audio/ac97.c`. Class 0x0401 with two I/O windows (mixer and bus master). Leaves cold reset, waits for the
primary codec, resets the mixer, sets master, headphone, mono and PCM volumes to 0 dB (AC'97 volumes are attenuations: 0 is
loudest), and loops the PCM-out engine over a 256 KiB ring of 32 descriptors; the poll keeps the last valid index one behind
the current one so the list is circular, and restarts the engine if it halted.

## libaudio

`#include <audio.h>` (and `<music.h>` for songs and modules, `<chip.h>` / `<modplay.h>` for each kind on its own), linked
into every program. `audio_open(flags)` returns NULL when the machine
has no sound (errno says why, `audio_strerror()` says it in words), `audio_set_format`, `audio_write` (blocking, or
`AUDIO_NONBLOCK` and it returns what fits), `audio_drain`, `audio_flush`, `audio_status`, `audio_get_master` /
`audio_set_master`; WAV files (`audio_wav_open` / `_read` / `_seek`, `audio_wav_save`); tone and noise generators; the
system sounds (`audio_event`, `audio_event_wait`, `audio_events_enabled`).

## System sounds

Four short sounds, synthesized on the spot (`user/lib/audio/src/events.c`): `login` (a rising bell: C E G, then a high C),
`dialog` (one small bell, E6), `error` (two low buzzes, A3 and E3), `shutdown` (the login tune backwards). They play from
`hakurei-shell` when the desktop opens (and on *Log out*, *Shut down*, *Restart*, where the machine waits for the tune to
end), from Kappa dialogs (`k_messagebox`, `k_confirm_dialog` with an error icon buzzes, `k_input_dialog`), from the login
screen (wrong password: error, shutdown buttons: the tune) and the lock screen (wrong password: error, unlocked: the bell).

The switch, first match wins: the environment variable `HAKUREI_SOUNDS` (`on` or `off`), the file `/etc/hakurei/sounds`
(`on` or `off`), `nosounds` or `sounds=off` on the kernel command line; otherwise they are on. `beep -E NAME` plays one the
way the desktop does, `beep -e NAME` plays it whatever the switch says. The test harness adds `nosounds` to every kernel
command line unless a test passes `Qemu(sounds=True)`.

## Songs and the synthesizer

A song is a text file (`.tms`, documented in the manual page `tms(5)`): a header (`title`, `mood`, `author`, `tempo`,
`loop`), optional instrument definitions, up to six channels (each with a starting instrument, pan and gain), named patterns
and an order list:

```
tms 1
title Shrine Steps at Dawn
tempo 84
channel lead  inst=fm-lead pan=-0.15
channel bass  inst=bass
pattern A
  lead: o5 e4 g8 a8 g4 e4 | o5 d4 e8 g8 e2
  bass: o2 [c4 g4]2 | [a4 >e4 <]2
order A A
```

The notes of a channel are written in a small music macro language: `c d e f g a b` with `+`/`-` and a length (`4` quarter,
`8` eighth, dots), `r` rests, `o4 < >` octaves, `l8` default length, `v12` volume, `q7` gate (staccato to legato), `@name`
instrument, `&` tie, `[ ... ]3` repeat, `w48` wait. Each pattern restarts every channel from `o4 l8 v12 q7` and its own
instrument; channels shorter than the pattern are padded with silence, and `play -i` reports it as a note so a song
is checked for bar alignment before anyone plays it.

The synthesizer (`user/lib/audio/src/chip.c`) gives every channel one monophonic voice: a pulse (50, 25 or 12.5 %, band
limited with PolyBLEP), triangle, saw or noise generator, or a two-operator FM voice (a modulator at a frequency ratio with
feedback bends the carrier's phase, and the bending fades by its own decay: bells, electric pianos, plucks, brass) under an
attack-decay-sustain-release envelope with optional vibrato. There is a ready-made bank of 19 instruments (`lead`, `pulse`,
`bass`, `hat`, `snare`, `kick`, `pad`, `pluck`, `tri-bell`, `thin`, `saw`, `open-hat`, `fm-bell`, `fm-epiano`, `fm-bass`,
`fm-pad`, `fm-pluck`, `fm-lead`, `fm-brass`); a song may define more with `inst N kind key=value ...`. Rendering is 32 kHz
stereo floating point in userland; in the QEMU guest it runs at about ten times real time, so playing and seeking are
cheap (`chip_seek` repositions the event cursors, nothing is replayed). One tempo per song, no tempo changes.

The songs that ship with the system are in `initrd/ScarletDevilMansion/share/music` (installed as
`/ScarletDevilMansion/share/music`): all original, written for this project in made-up scales and progressions, none of
them taken from or modelled on any Touhou melody (the project follows ZUN's fan-work guidelines, see ASSETS.md).

| File | Title | Tempo | Length | Character |
|---|---|---|---|---|
| `shrine-steps.tms` | Shrine Steps at Dawn | 84 | 1:32 | C major pentatonic, FM lead, music-box arpeggios, soft beat |
| `bamboo-lantern.tms` | Bamboo Lantern Walk | 96 | 1:40 | A minor, sixteenth-note arpeggios, FM bells, backbeat |
| `kappa-workshop.tms` | Kappa Workshop Rush | 132 | 1:20 | F major chiptune: square lead, pulse stabs, octave-bouncing bass |
| `moonlit-lake.tms` | Moonlit Lake Ripples | 66 | 1:49 | D minor, slow, FM electric piano, no drums |
| `netherworld-tea.tms` | Netherworld Tea Party | 104 | 1:03 | a waltz (3/4) in E minor and G, FM pluck like a koto |

Write your own with the Notebook, put it in `~/Music` and the Jukebox finds it; `play -i file.tms` checks it first.

## Tracker modules

`user/lib/audio/src/mod.c` (`modplay.h`) plays ProTracker modules, and `music.c` (`music.h`) is the one front that `play` and the
Jukebox use for both kinds: `music_open()` recognises a song or a module by its name or its first bytes (`tms` at the start, a
signature at byte 1080), `music_render()` gives 16-bit stereo frames, `music_seek()` and `music_length()` work for both.

* Read: the 31-sample format with the signatures `M.K.`, `M!K!`, `FLT4`, `4CHN`, `6CHN`, `8CHN`, `OCTA`, `CD81`, `xxCH`
  (10 to 32 channels) and `TDZx`. The whole file is read into memory (at most 12 MiB); a module that is cut short, or whose
  song length or signature is wrong, is refused with a message, a sample that is cut short is padded with silence.
* Played: arpeggio, slide up and down, tone portamento (also with volume slide), vibrato (also with volume slide), tremolo,
  sample offset, volume slide, position jump, set volume, pattern break, set speed and tempo, and from `Exy` fine slides, the
  vibrato and tremolo waveforms, finetune, pattern loop, retrigger, fine volume slides, note cut, note delay and pattern
  delay. Samples are mixed in floating point with linear interpolation at the period's rate (the PAL Amiga clock), channels
  are panned left, right, right, left with 30 % crosstalk, and the sum goes through a soft limiter.
* Not played: `E0x` filter, `E3x` glissando control, `E8x` panning, `EFx` funk repeat, 15-sample modules, S3M, XM and IT
  files, and the quirks of individual trackers (each plays a few effects a little differently). The effects follow the usual
  description of ProTracker's, written from memory; they were not compared with its replay routine sample by sample, and
  **no real tracker tune has been played through the player**: the tests use modules generated by `tests/modtools.py`.
* Tested, against a tick by tick model of each effect that is written down again in Python (`audio_mod_effects`): slide
  up and down, tone portamento (with its speed remembered), vibrato, tremolo, volume slide, speed and tempo, note cut,
  note delay, retrigger, sample offset, pattern loop, finetune, fine slides, position jump, pattern break, pattern delay,
  arpeggio, the end of a song that jumps back, channel panning, pitch and seeking (`audio_mod_module`). Not tested: the
  combined effects `5xy` and `6xy`, the vibrato and tremolo waveforms (`E4x`, `E7x`), fine volume slides (`EAx`, `EBx`), a
  pattern break to a row other than the first, and modules with more than four channels or with finetuned samples.
* The length is found by running the song once without mixing. A position jump back to a row that was already played is
  taken as the end of the song, so a tune that loops for ever is played once and has the length of one pass.
* Seeking runs the song's rows without mixing any sound, which is cheap; voices that were sounding at the target start at
  the right place inside their samples.

`tests/modtools.py` builds the test modules from scratch (`ModBuilder`: a 64-byte sine sample, a short burst, a sample that
starts with silence, patterns of 64 rows by 4 channels) so that every number a test checks can be worked out by hand.

## Reimu's Jukebox

`user/bin/jukebox/` (`jukebox.h` state, `player.c` loading, feeding and analysis, `playlist.c`, `ui.c`, `main.c`); listed in
the start menu as *Reimu's Jukebox*, window title "Jukebox - <song>". The window is the Prismriver Concert Hall: a stage with
curtains and spotlights where Lunasa (violin, the low part of the sound), Merlin (trumpet, the middle) and Lyrica (keyboard,
the high part) bob and glow with the music, and an orchestra pit below that is a 32 band spectrum with peak caps. On the left
the playlist (the shipped songs, then `~/Music/*.tms`, `*.mod` and `*.wav`, then anything opened with *Open music...*); under the stage
the title and mood, a seek bar with the time, previous / play-pause / stop / next, and the master volume with a mute
button. Keys and options are in `jukebox(1)`; `--trace` prints one line per event (the tests read those), `--list` prints the
playlist and exits.

How it plays: no thread and no helper process. The window's tick (45 ms while playing) asks the kernel how much is queued
(`AUDIO_GET_STATUS`), renders or reads more until about 450 ms are queued, and writes it with a non-blocking stream. The
position shown is what has been rendered minus what is still queued and in the hardware ring. Pausing, seeking and stopping
flush the stream (heard at once) and, on resume, seek the source to the remembered position. The levels come from a 512
point FFT of the audio around the position being heard. WAV files stream from disk (8/16/24/32-bit, mono or stereo, any
rate); a song that does not parse stays in the list with a warning icon and the parser's message.

## The panel's volume control

`user/bin/hakurei-shell/volume.c`: a speaker in the tray, drawn only if `/dev/audio` exists. Click for a small popup with a
slider and a Mute button, scroll over it to change the volume by 5, middle click to mute. It reads the kernel's master
volume twice a second, so `volume 30` in a terminal shows up there.

## Debugging on a real machine

* `cat /proc/audio`: state (idle, running, stalled), the controller and codec, every pin with its configuration word, jack
  sense and path, counters (mixed, played, underruns), one line per open stream.
* `dmesg | grep Nitori`: what the drivers decided. Kernel command line words: `noaudio` (no sound drivers), `audio.idle=MS`,
  `hda.dump`, `hda.imm`, `hda.posbuf`, `nosounds`.
* `volume`, `beep`, `beep -w noise -a 10`, `play -i song.tms` (needs no device), `play song.tms`.
* No sound from a jack that `/proc/audio` lists as `on`: check `volume` (not muted?), try the other jack, look at the `other
  pin` lines (a pin the BIOS declared unused is never driven) and send the `hda.dump` output.
* No `/dev/audio` and no `hda ...: looking for a codec` line in `dmesg | grep Nitori`: the driver only looks at PCI class
  0x0403 (HD Audio). If the BIOS presents the controller as class 0x0401 ("multimedia audio", the Intel DSP mode that some
  BIOSes offer), it is not looked at; `lspci`-style output of the machine (`cat /proc/pci` if there is one) would show it. Set
  the BIOS option to plain HD Audio, or extend `hda_init()` to try 0x0401 devices from Intel.

## Tests

`python tests/run_all.py audio_` runs `tests/case_audio.py` (about ten minutes; `tests/modtools.py` builds the tracker modules
it plays). The harness (`tools/qemu.py`) takes
`Qemu(audio="hda" | "hda-duplex" | "hda-micro" | "hda-nocodec" | "ac97", sounds=True|False)`: a sound card and an `-audiodev wav` backend writing
`build/<name>-audio.wav`; the file is complete once `vm.stop()` has run. The host side parses it with numpy
(`tests/audiotools.py`: loud segments, dominant frequency, peak, band power). The guest clock can be several times slower
than wall time, so tests never depend on how long the guest takes: they find the loud stretches of the recording and measure
those. `tests/guitext.py` finds text on a screenshot by rendering it with the desktop's own 8x16 font.

* `audio_hda_tone` -- a 440 Hz `beep` has the right pitch, level (0.5 at volume 100) and purity; `/proc/audio` names the card.
* `audio_mix_two_clients` -- two programs at once: both frequencies are present; two full-scale tones that beat against each
  other are limited below full scale and never wrap around.
* `audio_volume_and_mute` -- `volume` reads back, the quadratic curve, mute, relative changes, clamping, usage errors.
* `audio_format_conversion` -- mono 22050 Hz 8-bit, stereo 44100 Hz 16-bit and mono 11025 Hz 24-bit streams come out at the
  right pitch and level.
* `audio_underrun_is_silence` -- `play -g 2000 a.wav b.wav` keeps the stream open and starved for two seconds: the gap in the
  capture is exact silence, the underrun is counted, both tones are intact.
* `audio_no_device` -- no sound card: no `/dev/audio`, no `/proc/audio`, no `[Nitori]` line, and `beep`, `play` and `volume`
  say "no sound device" with status 1; `beep -o file.wav` still works.
* `audio_hda_no_codec` -- an HDA controller with nothing on its link (`hda-nocodec`): the driver logs "no codec answered",
  there is no `/dev/audio`, the tools say so, and the machine carries on.
* `audio_hda_micro_codec` -- QEMU's third codec, `hda-micro` (a speaker pin and a microphone pin): the speaker is chosen, the
  microphone is listed and left alone, and the tone is right.
* `audio_hda_graph` -- boots the guest and runs `hdatest` (`tests/host/hdatest.c`, built as the guest program
  `user/bin/hdatest` because the Windows host has no C library headers): 62 checks of the output planner on twelve codec
  descriptions, most of them invented Realtek-style trees (desktop and laptop shapes, a shared selector, loops, HDMI-only, a
  converter without 48 kHz, ...).
* `audio_hda_fallbacks` -- the duplex codec, immediate commands instead of CORB/RIRB, the position buffer instead of LPIB,
  `hda.dump`.
* `audio_songs` -- every shipped song parses, is bar-aligned (no padding notes), has a length of 45 s to 4 min and a sane
  level, and is listed by `jukebox --list`; broken song files give the right messages.
* `audio_song_timing_and_pitch` -- four quarter notes at 120 bpm are 0.5 s apart and A4, B4, C5; an FM whole note lasts about
  2 s and sounds at E5.
* `audio_mod_effects` -- a second generated module (`tests/modtools.py`) has one section for each effect, with silence
  between them; it is rendered to a WAV file with `play -w` (so the guest's speed cannot starve the playing), played, and
  the recording is compared with a model of what each effect does tick by tick: the measured pitches agree with the model to
  a tenth of a hertz, the tremolo peaks to three decimals, the speed, tempo, cut, delay and retrigger timings to 5 ms.
* `audio_mod_module` -- a module made by the test (one 64-byte sine sample, 4 channels, two patterns) is described by
  `play -i` (title, channels, 23 rows of 120 ms), listed by `jukebox --list`, recognised by its signature under another
  name and refused when it is junk; played, it has 259 Hz on the left, 518 Hz after the octave jump, 259 Hz on the right,
  then an arpeggio with three equal spectral peaks, and `play -s` lands on the same arpeggio.
* `audio_jukebox` -- starts the Jukebox on the desktop, sees the song title in the window (font-rendered match), checks that
  the music reaches the sound card, pauses, skips, seeks with the mouse, sets the volume with the mouse, mutes, quits; then
  starts it again with a generated `.mod` first in the list, which is heard at the right pitches and followed by the next song.
* `audio_system_sounds` -- the switch: `nosounds`, `HAKUREI_SOUNDS=on|off`, `beep -E` and `beep -e`; the bell is E6, the error
  sound is low, the shutdown tune ends on C5.
* `audio_desktop_sounds` -- the desktop plays the login chime when it opens; the login screen buzzes at a wrong password.
* `audio_panel_volume` -- click the speaker in the panel, drag the slider, `volume` agrees; Mute and Unmute.
* `audio_ac97_tone` -- the AC'97 path, with a 22050 Hz 8-bit client as well.

## Known limits and what is left

* Nothing has run on real hardware (see "Not verified" under Intel HDA). The first thing to do on the Dell is
  `cat /proc/audio`, `dmesg | grep Nitori` and `beep`.
* The songs were never listened to; they are written to be bar-aligned, in key, and at sane levels, which is what is
  measured. The mix is quiet at the default master volume of 60 (peaks around 0.2 of full scale): turn it up.
* Only ProTracker `.mod` files are read (see Tracker modules); S3M, XM and IT are not, and the `.mod` player has been
  heard by no one: no real tracker tune has been played through it. One tempo per `.tms` song. Voices are monophonic
  (chords are written as arpeggios or on several channels). No recording, no per-application volume in the panel (the
  Jukebox and the panel move the master volume).
* `play -g` exists because gapsh's `( a; b ) | cmd` closes the pipe after the first command of the group; not looked into.
* A guest that the host starves for more than a second can still make the hardware lap the ring and repeat old sound
  (`underruns` in `/proc/audio` counts the dry spells).
