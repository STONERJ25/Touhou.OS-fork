# Sound

The sound system belongs to the Kappa/Nitori hardware crowd (kernel log tag `Nitori`). Reimu's Jukebox is the music player.

```
 programs: beep play volume jukebox desktop sounds   user/bin/kappabox/audio.c, user/bin/jukebox/
        |  libaudio  (device, WAV, generators, chiptune synth, system sounds)    user/lib/audio/
        v  /dev/audio (write PCM, ioctls) and /proc/audio
 audio core  kernel/drivers/audio/audio.c      streams, software mixer, volume, limiter, mixer thread
        |  AudioBackend: a looping 48 kHz 16-bit stereo DMA ring + "where is the hardware now?"
        v
 hda.c  Intel High Definition Audio (QEMU intel-hda, the Dell's 8086:a171 + Realtek codec)      ac97.c  AC'97 (QEMU AC97)
```

## Status (read this first)

| Piece | State |
|---|---|
| Audio core: `/dev/audio`, mixer, per-stream and master volume, mute, soft limiter, `/proc/audio`, underruns are silence | done, tested in QEMU |
| Intel HDA driver (CORB/RIRB polled, codec graph parse, path selection, unmute, EAPD, stream, jack auto-mute) | done, tested in QEMU with `hda-output` only; **never run on real hardware** |
| AC'97 driver (polled, circular descriptor list) | done, tested in QEMU (`AC97`); never run on real hardware |
| libaudio: device, WAV reader/writer, tone and noise generators, system sounds | done |
| libaudio: chiptune synthesizer / sequencer (`chip.h`, `chip.c`: MML notation, 12 instruments, ADSR, vibrato, PolyBLEP) | written and compiles; **never run or tested**, not used by any program yet |
| the procedural tracks (`chip_songs()`, `songs.c`) | not written yet |
| `beep`, `play` (`-g MS` keeps the stream open between files), `volume` | done |
| Reimu's Jukebox (GUI) | not written yet |
| System sounds hooked into the shell, greeter, lock screen and dialogs | not hooked in yet (`beep -e login` plays them) |
| Recording (capture) | not planned for the first round |
| Tests | `tests/case_audio.py` (see the list below) |

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
`AUDIO_FLUSH` throws the queue away; `AUDIO_DRAIN` waits until everything has been heard.

ioctls are in `shared/audio_abi.h`: `AUDIO_SET_FORMAT`, `AUDIO_GET_FORMAT`, `AUDIO_SET_VOLUME`, `AUDIO_GET_VOLUME`,
`AUDIO_SET_MASTER`, `AUDIO_GET_MASTER`, `AUDIO_GET_INFO`, `AUDIO_GET_STATUS`, `AUDIO_DRAIN`, `AUDIO_FLUSH`. No new system
calls are used (the numbers 100-105 stay free).

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

`kernel/drivers/audio/hda.c`. Polled; there are no interrupts and no MSI.

1. PCI class 0x0403. Memory and bus master enabled; on Intel, traffic class 0 and snooped DMA (what Linux does).
2. Controller reset, wait for the codec bits in `STATESTS`, CORB/RIRB set up (256 entries when the controller allows it).
   The response flag in `RIRBSTS` is acknowledged after every command (QEMU stops reading commands until it is).
3. For each codec: the audio function group is powered to D0, then every widget is read: type, capabilities, connection
   list (short and long form, ranges), pin capabilities, pin default configuration, amplifier capabilities (with the
   function group's values where the widget does not override them).
4. Output pins are the ones the BIOS declared as line out, speaker or headphone and that are not "not connected". For each
   a path pin <- (mixers, selectors) <- DAC is found by depth-first search, preferring a DAC no other pin uses. If the BIOS
   declared nothing, any output-capable pin with a path is used (with a log line).
5. Along every path: widgets that have power control go to D0, selectors and multi-input pins are pointed at the path,
   mixer inputs other than the path's are muted, output amplifiers are unmuted at their 0 dB step, the pin is enabled
   (`OUT_EN`, plus `HP_EN` on headphone drive capable pins) and its EAPD is switched on.
6. One output stream, tag 1, 48 kHz 16-bit stereo, over a 256 KiB ring described by 16 buffer descriptors. All DACs used
   by any enabled path listen to the same stream.
7. Jack sensing (`GET_PIN_SENSE`, polled four times a second while playing): headphones plugged in silence the speaker
   and the rear line out; a plugged line out silences the speaker. Pins without presence detect count as plugged.

Failure is quiet: if a codec stops answering, if there is no analog output (an HDMI-only codec), or if the controller
will not reset, the driver logs one `[Nitori]` line and `/dev/audio` does not appear. Nothing is ever played on a pin the
BIOS marked unused, and the default master volume is moderate.

What has **not** been done: vendor specific coefficient programming (Realtek `COEF` tweaks some laptops need), GPIO
controlled external amplifiers, digital (S/PDIF, HDMI) outputs, input streams (recording), power saving of idle widgets.
The path logic follows the specification and Linux's generic parser, but it has only ever run against QEMU's simple
codec. If the Dell's front or rear jack stays silent, `cat /proc/audio` lists the codec, each pin with its configuration
word, whether a jack is sensed, the path chosen, and `dmesg | grep Nitori` shows what the driver decided.

## Tests

`python tests/run_all.py audio` runs `tests/case_audio.py`. The harness (`tools/qemu.py`) takes
`Qemu(audio="hda" | "hda-duplex" | "ac97")`, which adds a sound card and an `-audiodev wav` backend writing
`build/<name>-audio.wav`; the file is complete once `vm.stop()` has run. The host side parses it with numpy
(`tests/audiotools.py`: loud segments, dominant frequency, peak, band power). The guest clock can be several times slower
than wall time, so tests never depend on durations: they find the loud stretches and measure those. System sounds are
switched off in the harness (`nosounds` is added to the command line) unless a test passes `sounds=True`.

Covered so far (all on QEMU's `hda-output`; `python tests/run_all.py audio_`):

* `audio_hda_tone` -- a 440 Hz `beep` has the right pitch, level (0.5 at volume 100) and purity; `/proc/audio` names the card.
* `audio_mix_two_clients` -- two programs at once: both frequencies are present at the right level; two full-scale tones
  that beat against each other are limited below full scale and never wrap around.
* `audio_volume_and_mute` -- `volume` reads back, the quadratic curve (50 gives a quarter of the amplitude), mute, relative
  changes, clamping, usage errors.
* `audio_format_conversion` -- mono 22050 Hz 8-bit, stereo 44100 Hz 16-bit and mono 11025 Hz 24-bit streams come out at the
  right pitch and level.
* `audio_underrun_is_silence` -- `play -g 2000 a.wav b.wav` keeps the stream open and starved for two seconds: the gap in the
  capture is exact silence, the underrun is counted in `/proc/audio`, both tones are intact.
* `audio_no_device` -- no sound card: no `/dev/audio`, no `/proc/audio`, no `[Nitori]` log line, and `beep`, `play`
  and `volume` say "no sound device" with status 1; `beep -o file.wav` still works.

Also `audio_ac97_tone` (the AC'97 path with a 22050 Hz 8-bit client as well).

Not covered yet: `hda-duplex`, the Jukebox, the system sounds in the desktop, the real Dell.

## State when paused

The work was stopped on request in the middle of the sound project; this is exactly where it is.

**Done, committed, tested in QEMU** (`python tests/run_all.py audio_` passed 7 of 7 and `manual` passed, on the commit before
the last merge of `main`; after that merge only `audio_hda_tone`, `audio_no_device` and `audio_ac97_tone` were re-run and passed):
the audio core (`/dev/audio`, mixer, limiter, volumes, `/proc/audio`, silence on underrun, stall fallback), the Intel HDA
driver, the AC'97 driver, libaudio's device/WAV/generator/system-sound code, the `beep`, `play`, `volume` applets with
manual pages (`beep(1)`, `play(1)`, `volume(1)`, `audio(7)`), the harness support (`Qemu(audio=...)`, `tests/audiotools.py`).

**Half done:** `user/lib/audio/include/chip.h` and `src/chip.c`, the chiptune synthesizer and MML sequencer. It compiles
without warnings but has never been run: nothing calls it, and `chip_songs()` (declared in `chip.h`) does not exist yet.
Expect small bugs; test it first on the host (it is plain C with libm: compile `chip.c` with a host compiler together with
a `main` that renders a song to a WAV, and look at the file with `tests/audiotools.py`).

**Not started:**

* `user/lib/audio/src/songs.c`: five or so original tracks in MML (instrument table and the notation are in `chip.h`).
* Reimu's Jukebox (`user/bin/jukebox/`, a Kappa app): track list (the built-in songs and the `.wav` files in `~/Music`), play,
  pause, stop, next, seek bar, volume slider (the master volume), a spectrum or waveform view, a `.pw` manual page, an entry
  in `user/bin/hakurei-shell/launch.c` and a GUI test with `tests/guitools.py`. Plan: feed `/dev/audio` from the `kapp_set_tick`
  callback with a non-blocking stream, about 400 ms ahead, using `AUDIO_GET_STATUS` to see how much is queued; `AUDIO_FLUSH`
  on pause and seek.
* Hooking the system sounds in: `audio_event(AUDIO_EV_LOGIN)` in `hakurei-shell` at start, `AUDIO_EV_DIALOG` and
  `AUDIO_EV_ERROR` in the Kappa dialogs, `AUDIO_EV_SHUTDOWN` before `gap_reboot` (wait about a second so it is heard). The
  functions, the switch and the sounds exist and work (`beep -e login`); no program calls them. A test with
  `Qemu(sounds=True)` should check that the chime reaches the capture.
* Recording, an `hda-duplex` test, mentions in `docs/DESKTOP.md` and `docs/THEMING.md` beyond the lines already added.

**Known problems and risks:**

* Nothing has run on real hardware. Everything about the Dell (jack retasking, Realtek coefficient tweaks, GPIO amplifiers,
  the HDMI codec on the same controller) is untested. The HDA driver has no immediate-command fallback if CORB/RIRB fails.
* The mix test (`audio_mix_two_clients`) was flaky until the ring was lengthened to 1.4 s; a guest that the host starves for
  more than a second can still make the hardware lap the ring and repeat old sound. If it flakes again, lengthen the ring
  or look at the `underruns` counters in `/proc/audio`.
* `play -g` exists because gapsh's `( a; b ) | cmd` closes the pipe after the first command of the group; not looked into.
* The mixer thread polls every 5 ms while sound plays and sleeps otherwise; the hardware engine stops after 3 s of silence
  (`audio.idle=MS` on the command line). The first sound after that has a short start-up.

**How to resume:** `git log` on this branch, `python tools/build.py`, `python tests/run_all.py audio_` (about two minutes).
Merge `main` first (`git merge main`; `tools/qemu.py` is the file that conflicts: keep both the `boot_image` and the `audio`
parameters). Then do the "Not started" list in this order: songs.c, a host-side render of every song, `play --song`, the
Jukebox, the hooks, the tests, the full `python tests/run_all.py`.
