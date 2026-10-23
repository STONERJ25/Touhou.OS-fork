"""Sound: the Intel HDA and AC'97 drivers, the mixer behind /dev/audio, and the tools on top of it.

QEMU's sound card plays into a WAV file (Qemu(audio=...)); the tests measure that file on the host with numpy. The guest
clock can be several times slower than wall time, so nothing here depends on how long a sound lasts: tests wait for the
shell to come back, stop the VM, and look at the loud stretches of the recording."""
import re
import time

import numpy as np

import audiotools as at
import modtools as mt
import test_shell
from harness import test, Qemu, BUILD


def sh(vm, cmd, timeout=90):
    return test_shell.run(vm, cmd, timeout=timeout)


def capture(vm):
    """Stops the VM so QEMU finishes the WAV, and returns (rate, mono samples)."""
    vm.stop()
    rate, x = at.load(vm.audio_path)
    return rate, at.mono(x)


def loudest(x, rate, **kw):
    """The longest loud stretch of a capture."""
    segs = at.segments(x, rate, **kw)
    assert segs, "nothing was played"
    a, b = max(segs, key=lambda s: s[1] - s[0])
    return x[a:b]


@test
def audio_hda_tone():
    """Intel HDA: beep plays a 440 Hz tone; the sound card output has that pitch and the expected level."""
    with Qemu(cmdline="text", name="t-audio-hda", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "cat /proc/audio")
        assert rc == 0 and "Intel HD Audio" in out, out
        out, rc = sh(vm, "volume 100 && beep -f 440 -l 1200 -a 50")
        assert rc == 0, out
        out, rc = sh(vm, "cat /proc/audio")
        assert re.search(r"underruns \d+", out), out
        rate, x = capture(vm)
    assert rate == 48000, rate
    segs = at.segments(x, rate)
    assert segs, "nothing was played"
    a, b = max(segs, key=lambda s: s[1] - s[0])
    tone = at.trimmed(x[a:b], rate)
    f = at.dominant_freq(tone, rate)
    assert abs(f - 440) < 4, f"dominant frequency {f:.1f} Hz, expected 440"
    p = at.peak(tone)
    assert 0.42 < p < 0.55, f"peak amplitude {p:.3f}, expected about 0.5"
    assert at.band_power(tone, rate, 440) > 0.9, "the tone is not clean"


@test
def audio_mix_two_clients():
    """Two programs play at the same time: both tones are in the output, and loud sums are limited, never wrapped."""
    with Qemu(cmdline="text", name="t-audio-mix", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "volume 100 && beep -f 440 -l 1500 -a 30 & beep -f 660 -l 1500 -a 30 & wait; echo both-done")
        assert "both-done" in out, out
        out, rc = sh(vm, "sleep 1; beep -f 440 -l 4000 -a 100 & beep -f 441 -l 4000 -a 100 & wait; echo loud-done")
        assert "loud-done" in out, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, merge_ms=400)
    assert len(segs) >= 2, f"expected two sounds, found {segs}"
    a, b = segs[0]
    both = at.trimmed(x[a:b], rate)
    p440, p660 = at.band_power(both, rate, 440, 20), at.band_power(both, rate, 660, 20)
    assert p440 > 0.25 and p660 > 0.25, f"440 Hz has {p440:.2f} of the power, 660 Hz {p660:.2f}: not both mixed"
    assert 0.35 < at.peak(both) < 0.7, f"peak {at.peak(both):.2f}, two tones of 0.3 should reach at most 0.6"
    a, b = segs[-1]
    loud = x[a:b]
    assert at.peak(loud) <= 1.0, "the limiter let a sample through above full scale"
    assert at.peak(loud) > 0.8, f"the loud mix is only {at.peak(loud):.2f}"
    jumps = float(abs(loud[1:] - loud[:-1]).max())
    assert jumps < 0.35, f"a jump of {jumps:.2f} between samples: the sum wrapped around instead of being limited"


@test
def audio_volume_and_mute():
    """volume sets the master level (quadratic curve) and mute silences the output; the settings read back."""
    with Qemu(cmdline="text", name="t-audio-vol", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "volume 100; volume")
        assert rc == 0 and "Master volume: 100%" in out, out
        sh(vm, "beep -f 440 -l 1000 -a 40")
        out, rc = sh(vm, "volume 50; volume")
        assert "Master volume: 50%" in out, out
        sh(vm, "sleep 1; beep -f 440 -l 1000 -a 80")
        out, rc = sh(vm, "volume mute; volume")
        assert "(muted)" in out, out
        sh(vm, "sleep 1; beep -f 440 -l 1000 -a 80")
        out, rc = sh(vm, "volume unmute; volume -20; volume")
        assert "Master volume: 30%" in out and "muted" not in out, out
        out, rc = sh(vm, "volume 200; echo rc=$?")
        assert "Master volume: 100%" in out, out
        out, rc = sh(vm, "volume loud; echo rc=$?")
        assert "rc=2" in out, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.005, merge_ms=400)
    assert len(segs) == 2, f"expected two audible sounds (the muted one is silent), found {segs}"
    full = at.peak(at.trimmed(x[segs[0][0]:segs[0][1]], rate))
    half = at.peak(at.trimmed(x[segs[1][0]:segs[1][1]], rate))
    assert 0.35 < full < 0.45, f"100% volume, amplitude 40%: peak {full:.3f}"
    assert 0.15 < half < 0.25, f"50% volume (gain 0.25), amplitude 80%: peak {half:.3f}, expected 0.2"


@test
def audio_format_conversion():
    """A mono 22050 Hz 8-bit stream is converted to 48 kHz stereo 16-bit at the right pitch; 44100 Hz 16-bit too."""
    with Qemu(cmdline="text", name="t-audio-conv", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        sh(vm, "volume 100")
        out, rc = sh(vm, "beep -r 22050 -c 1 -b 8 -f 440 -l 1200 -a 50")
        assert rc == 0, out
        out, rc = sh(vm, "sleep 1; beep -r 44100 -c 2 -b 16 -f 1000 -l 1200 -a 50")
        assert rc == 0, out
        out, rc = sh(vm, "sleep 1; beep -r 11025 -c 1 -b 24 -f 300 -l 1200 -a 50")
        assert rc == 0, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, merge_ms=400)
    assert len(segs) == 3, f"expected three sounds, found {segs}"
    for (a, b), want in zip(segs, (440, 1000, 300)):
        tone = at.trimmed(x[a:b], rate)
        f = at.dominant_freq(tone, rate)
        assert abs(f - want) < want * 0.01 + 2, f"expected {want} Hz, heard {f:.1f} Hz"
        assert 0.4 < at.peak(tone) < 0.6, f"{want} Hz: peak {at.peak(tone):.2f}"


@test
def audio_underrun_is_silence():
    """A stream that runs dry while the hardware keeps playing produces silence, never a repeat of old sound."""
    with Qemu(cmdline="text", name="t-audio-under", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        sh(vm, "volume 100")
        sh(vm, "beep -f 440 -l 500 -o /tmp/a.wav; beep -f 880 -l 500 -o /tmp/b.wav")
        out, rc = sh(vm, "play -q -g 2000 /tmp/a.wav /tmp/b.wav", timeout=120)      # -g: wait 2 s between the files with the stream still open
        assert rc == 0, out
        out, rc = sh(vm, "cat /proc/audio")
        assert re.search(r"underruns [1-9]", out), "the underrun was not counted:\n" + out
        rate, x = capture(vm)
    segs = at.segments(x, rate, merge_ms=300)
    assert len(segs) == 2, f"expected two tones with a gap, found {segs}"
    (a1, b1), (a2, b2) = segs
    gap = x[b1 + rate // 50:a2 - rate // 50]
    assert len(gap) > rate * 0.8, f"the gap is only {len(gap) / rate:.2f} s"
    assert at.peak(gap) < 0.002, f"the gap is not silent (peak {at.peak(gap):.4f}): old sound was repeated"
    assert abs(at.dominant_freq(at.trimmed(x[a1:b1], rate), rate) - 440) < 5
    assert abs(at.dominant_freq(at.trimmed(x[a2:b2], rate), rate) - 880) < 9


@test
def audio_no_device():
    """Without sound hardware /dev/audio does not exist and the tools say so clearly."""
    with Qemu(cmdline="text", name="t-audio-none") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "ls /dev/audio /proc/audio")
        assert rc != 0 and "No such file" in out, out
        for cmd in ("beep", "play /tmp/none.wav", "volume", "volume 50"):
            out, rc = sh(vm, f"{cmd} 2>&1")
            assert "no sound device" in out, f"{cmd}: {out}"
        out, rc = sh(vm, "beep 2>/dev/null; echo rc=$?")
        assert "rc=1" in out, out
        out, rc = sh(vm, "beep -f 440 -l 100 -o /tmp/x.wav && ls -l /tmp/x.wav")
        assert rc == 0 and "x.wav" in out, out
        out, rc = sh(vm, "dmesg | grep -c 'Nitori'")
        assert out.strip() == "0", out


@test
def audio_hda_no_codec():
    """An HDA controller with no codec behind it: the boot is not held up, there is no /dev/audio, the log says why."""
    with Qemu(cmdline="text", name="t-audio-nocodec", audio="hda-nocodec") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "ls /dev/audio /proc/audio")
        assert rc != 0 and "No such file" in out, out
        out, rc = sh(vm, "dmesg | grep Nitori")
        assert "no codec answered" in out, out
        out, rc = sh(vm, "beep 2>&1; echo rc=$?")
        assert "no sound device" in out and "rc=1" in out, out
        out, rc = sh(vm, "echo still-alive")
        assert "still-alive" in out, out


@test
def audio_hda_micro_codec():
    """QEMU's third codec, hda-micro (a speaker pin and a microphone pin): the planner picks the speaker and the tone is right."""
    with Qemu(cmdline="text", name="t-audio-micro", audio="hda-micro") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "cat /proc/audio")
        assert "speaker(0x03<-0x02)" in out, out                 # pin 0x03 is fed by the converter 0x02
        assert "other pin 0x05: mic" in out, out                 # the microphone is listed, and not driven
        out, rc = sh(vm, "volume 100 && beep -f 523 -l 1000 -a 50")
        assert rc == 0, out
        rate, x = capture(vm)
    tone = at.trimmed(loudest(x, rate), rate)
    assert abs(at.dominant_freq(tone, rate) - 523) < 6, at.dominant_freq(tone, rate)
    assert 0.42 < at.peak(tone) < 0.55, at.peak(tone)


@test
def audio_hda_graph():
    """The HDA output planner picks the right pins, mixers, selectors and converters in invented Realtek-style codecs."""
    with Qemu(cmdline="text", name="t-audio-graph") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "hdatest")
        assert rc == 0 and re.search(r"\d+ checks, 0 failed", out), out
        for fixture in ("qemu hda-output", "alc662-like desktop", "alc255-like desktop", "alc255-like, internal speaker"):
            assert fixture in out, f"{fixture!r} missing from:\n{out}"


@test
def audio_hda_fallbacks():
    """The duplex codec works, and so do the fallbacks: immediate commands instead of CORB/RIRB, the position buffer instead of LPIB."""
    with Qemu(cmdline="text hda.imm hda.posbuf hda.dump", name="t-audio-fallback", audio="hda-duplex") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "cat /proc/audio")
        assert "commands by the immediate registers" in out and "position from the position buffer" in out, out
        assert "line out" in out and "output:   pin" in out, out
        out, rc = sh(vm, "dmesg | grep 'hda:  0x'")
        assert out.count("hda:  0x") >= 4 and "type 4" in out and "config 00004010" in out, out        # hda.dump: every widget of the codec
        out, rc = sh(vm, "volume 100 && beep -f 660 -l 1500 -a 50")
        assert rc == 0, out
        rate, x = capture(vm)
    tone = at.trimmed(loudest(x, rate), rate)
    assert abs(at.dominant_freq(tone, rate) - 660) < 7, at.dominant_freq(tone, rate)
    assert 0.42 < at.peak(tone) < 0.55, at.peak(tone)
    assert float(abs(tone[1:] - tone[:-1]).max()) < 0.2, "the output has a jump in it"


SONGS = ("Bamboo Lantern Walk", "Kappa Workshop Rush", "Moonlit Lake Ripples", "Netherworld Tea Party", "Shrine Steps at Dawn")


@test
def audio_songs():
    """The songs that ship with the system parse, are bar-aligned, have a sensible length and level, and show up in the Jukebox list."""
    with Qemu(cmdline="text", name="t-audio-songs") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "jukebox --list")
        assert rc == 0, out
        for i, title in enumerate(SONGS, 1):
            assert re.search(rf"^\s*{i}\. {re.escape(title)} \(\d+:\d\d\) /ScarletDevilMansion/share/music/\S+\.tms$", out, re.M), f"{title!r} missing from:\n{out}"
        for path in re.findall(r"(/ScarletDevilMansion/share/music/\S+\.tms)", out):
            info, rc = sh(vm, f"play -i -t 10 {path}", timeout=180)
            assert rc == 0, info
            assert "note:" not in info, f"{path}: a channel does not fill its pattern: {info}"
            m = re.search(r"(\d+) bpm, (\d) channels?, (\d+):(\d\d)", info)
            assert m and 45 <= int(m.group(3)) * 60 + int(m.group(4)) <= 240, f"{path}: length: {info}"
            m = re.search(r"peak ([\d.]+), rms ([\d.]+), (\d+) samples above 0.9, (\d+)% silent", info)
            assert m, info
            assert 0.2 < float(m.group(1)) < 0.9 and float(m.group(2)) > 0.04 and int(m.group(4)) < 30, f"{path}: {info}"
        out, rc = sh(vm, "printf 'tms 1\\ntempo 120\\nchannel a inst=nope\\n' > /tmp/bad.tms; play -i /tmp/bad.tms")
        assert rc == 1 and "no instrument 'nope'" in out, out
        out, rc = sh(vm, "printf 'tms 1\\nchannel a inst=bass\\npattern P\\n  a: o4 c4 x4\\n' > /tmp/bad2.tms; play -i /tmp/bad2.tms")
        assert rc == 1 and "cannot read the notation" in out, out


@test
def audio_song_timing_and_pitch():
    """A song written in the notation plays at the right tempo and pitch: four quarter notes at 120 bpm, A4 to C5, FM and triangle."""
    with Qemu(cmdline="text", name="t-audio-pitch", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        sh(vm, "volume 100")
        sh(vm, "printf 'tms 1\\ntempo 120\\nchannel a inst=bass\\npattern P\\n  a: q5 o4 a4 b4 >c4 <a4\\norder P\\n' > /tmp/tri.tms")
        out, rc = sh(vm, "play -q /tmp/tri.tms", timeout=120)
        assert rc == 0, out
        sh(vm, "printf 'tms 1\\ntempo 120\\nchannel a inst=fm-epiano\\npattern P\\n  a: o5 e1\\norder P\\n' > /tmp/fm.tms")
        out, rc = sh(vm, "sleep 1; play -q /tmp/fm.tms", timeout=120)
        assert rc == 0, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.004, merge_ms=60, min_ms=100)
    assert len(segs) >= 5, f"expected four notes and a long one, found {segs}"
    notes, longer = segs[:4], segs[4]
    starts = [a / rate for a, _ in notes]
    for i in range(3):
        assert abs((starts[i + 1] - starts[i]) - 0.5) < 0.03, f"quarter notes at 120 bpm are 0.5 s apart, got {starts}"
    for (a, b), want in zip(notes[:3], (440.0, 493.9, 523.3)):
        f = at.dominant_freq(at.trimmed(x[a:b], rate, ms=60), rate)
        assert abs(f - want) < want * 0.012, f"expected {want} Hz, heard {f:.1f}"
    a, b = longer
    assert 1.6 < (b - a) / rate < 3.0, f"a whole note at 120 bpm lasts about 2 s (gate 7/8, plus its release): {(b - a) / rate:.2f}"
    f = at.dominant_freq(at.trimmed(x[a:b], rate, ms=60), rate)
    assert abs(f - 659.3) < 8, f"the FM note should sound at E5 (659 Hz), heard {f:.1f}"


def put_file(vm, path, data, chunk=200):
    """Writes a file into the guest through the shell: gzipped, as base64 text in short lines, decoded there and checked
    with md5sum (the guest's line editor echoes a whole line again for every key, so lines are kept short)."""
    import base64
    import gzip
    import hashlib
    text = base64.b64encode(gzip.compress(data, 9, mtime=0)).decode()
    sh(vm, f"rm -f {path}.b64 {path}.gz")
    for i in range(0, len(text), chunk):
        out, rc = sh(vm, f"echo {text[i:i + chunk]} >> {path}.b64")
        assert rc == 0, out
    out, rc = sh(vm, f"base64 -d {path}.b64 > {path}.gz && zcat {path}.gz > {path} && md5sum {path}")
    assert hashlib.md5(data).hexdigest() in out, f"the file did not arrive intact: {out}"


def rms(x):
    return float((x.astype("float64") ** 2).mean() ** 0.5) if len(x) else 0.0


@test
def audio_mod_module():
    """A ProTracker module plays at the right pitch, speed and side; arpeggio, pattern delay and seeking work; junk is refused."""
    mod = mt.make_module()
    with Qemu(cmdline="text", name="t-audio-mod", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        put_file(vm, "/tmp/test.mod", mod)
        out, rc = sh(vm, "play -i /tmp/test.mod", timeout=180)
        assert rc == 0, out
        assert "Test Module (ProTracker module, 4 channels)" in out, out
        assert re.search(r"module, 4 channels, 0:03", out), out
        m = re.search(r"measured ([\d.]+) s: peak ([\d.]+)", out)
        assert m and abs(float(m.group(1)) - 2.8) < 0.06 and 0.2 < float(m.group(2)) < 0.28, f"23 rows of 120 ms were expected: {out}"
        out, rc = sh(vm, "jukebox --list /tmp/test.mod")
        assert re.search(r"^\s*1\. Test Module \(0:03\) /tmp/test\.mod$", out, re.M), out
        out, rc = sh(vm, "cp /tmp/test.mod /tmp/renamed.bin; play -i /tmp/renamed.bin")           # found by its signature, not its name
        assert rc == 0 and "Test Module" in out, out
        out, rc = sh(vm, "echo hello > /tmp/junk.mod; play -i /tmp/junk.mod")
        assert rc == 1 and "too short to be a ProTracker module" in out, out
        sh(vm, "volume 100")
        out, rc = sh(vm, "play -q /tmp/test.mod", timeout=180)
        assert rc == 0, out
        out, rc = sh(vm, "sleep 1; play -q -s 1.56 -t 0.96 /tmp/test.mod", timeout=180)          # seek to the start of pattern 1
        assert rc == 0, out
        vm.stop()
        rate, x = at.load(vm.audio_path)
    left, right = x[:, 0], x[:, 1]
    segs = at.segments(x, rate, threshold=0.01, merge_ms=300)
    assert len(segs) == 2, f"expected the module and the part played after the seek, found {segs}"
    (a, b), (c, d) = segs
    assert 2.35 < (b - a) / rate < 2.7, f"rows 0-11 and 13-20 are 2.52 s of sound, found {(b - a) / rate:.2f} s"

    def win(ch, start, t0, t1):
        return ch[start + int(t0 * rate):start + int(t1 * rate)]

    f = at.dominant_freq(win(left, a, 0.06, 0.42), rate)
    assert abs(f - 258.97) < 4, f"period 214 with a 64-byte wave is 259 Hz, heard {f:.1f}"
    assert rms(win(left, a, 0.06, 0.42)) > 2.5 * rms(win(right, a, 0.06, 0.42)), "channel 1 belongs on the left"
    f = at.dominant_freq(win(left, a, 0.54, 0.90), rate)
    assert abs(f - 517.9) < 8, f"period 107 is an octave higher (518 Hz), heard {f:.1f}"
    f = at.dominant_freq(win(right, a, 1.02, 1.38), rate)
    assert abs(f - 258.97) < 4, f"channel 2 plays 259 Hz again, heard {f:.1f}"
    assert rms(win(right, a, 1.02, 1.38)) > 2.5 * rms(win(left, a, 1.02, 1.38)), "channel 2 belongs on the right"
    assert 0.2 < at.peak(left) < 0.3, f"one voice at full volume should peak near 0.24, found {at.peak(left):.2f}"

    def arpeggio(start, t0, t1):
        bands = [at.band_power(win(left, start, t0, t1), rate, hz, 14) for hz in (258.97, 307.9, 387.6)]
        assert min(bands) > 0.09 and sum(bands) > 0.45, f"an arpeggio of 0, 3 and 7 semitones has three equal peaks, found {bands}"

    arpeggio(a, 1.62, 2.46)                                                               # pattern 1 in the whole module
    assert 0.85 < (d - c) / rate < 1.15, f"the seek should leave the 0.96 s of pattern 1, found {(d - c) / rate:.2f} s"
    arpeggio(c, 0.06, 0.90)                                                               # the same after the seek
    assert rms(win(right, c, 0.06, 0.90)) < 0.45 * rms(win(left, c, 0.06, 0.90)), "after the seek only channel 1 is left"


@test
def audio_mod_effects():
    """Module effects follow a tick by tick model: slides, portamento, vibrato, tremolo, volume, speed, tempo, cut, delay, retrigger, offset, loop, finetune, jump."""
    mod = mt.make_effects_module()
    with Qemu(cmdline="text", name="t-audio-fx", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        put_file(vm, "/tmp/fx.mod", mod)
        out, rc = sh(vm, "play -i /tmp/fx.mod", timeout=240)
        assert rc == 0 and re.search(r"measured \d+\.\d s", out), f"a song that jumps back to a row it played is over: {out}"
        out, rc = sh(vm, "play -q -w /tmp/fx.wav /tmp/fx.mod", timeout=240)             # rendered first: the guest's speed cannot starve the playing
        assert rc == 0, out
        sh(vm, "volume 100")
        out, rc = sh(vm, "play -q /tmp/fx.wav", timeout=300)
        assert rc == 0, out
        vm.stop()
        rate, x = at.load(vm.audio_path)
    left, mono = x[:, 0], at.mono(x)
    segs = at.segments(x, rate, threshold=0.01, merge_ms=200, min_ms=20)
    names = mt.FX_SECTIONS
    assert len(segs) == len(names), f"expected one stretch of sound for each of the {len(names)} sections, found {len(segs)}: {segs}"
    PEAK = 0.2404                                                                         # one voice at full volume, as measured by audio_mod_module

    def start(i):
        """Where section i really starts: its first 5 ms window of sound is found, now the first loud sample in it."""
        a = segs[i][0]
        return a + int(np.argmax(np.abs(mono[a:a + rate // 100]) > 0.01))

    def f_in(i, t0, t1):
        a = start(i)
        return mt.mean_freq(left[a + int(t0 * rate):a + int(t1 * rate)], rate)

    def peak_in(i, t0, t1):
        a = start(i)
        return at.peak(left[a + int(t0 * rate):a + int(t1 * rate)])

    def runs(i):
        """The stretches of sound inside section i as (start, end) in milliseconds, from its first sound."""
        a, b = start(i), segs[i][1]
        return [(s * 1000 / rate, e * 1000 / rate) for s, e in at.segments(mono[a:b], rate, threshold=0.01, merge_ms=3, min_ms=2)]

    def row(r):                                                                           # inside row r, away from its edges (s)
        return r * 0.12 + 0.012, r * 0.12 + 0.108

    def near(i, what, value, want, tol):
        assert abs(value - want) <= tol * want, f"{names[i]}, {what}: expected {want:.1f}, found {value:.1f}"

    def about(i, what, value, want, ms):
        assert abs(value - want) <= ms, f"{names[i]}, {what}: expected {want:.0f} ms, found {value:.0f} ms (runs {runs(i)})"

    S = {n: i for i, n in enumerate(names)}

    i = S["slide up"]                                                                     # 1xx: 4 per tick on ticks 1-5 of each row
    t = mt.slide_ticks(428, -4, 4)
    for r in (0, 3):
        near(i, f"row {r}", f_in(i, *row(r)), mt.tick_hz(t[6 * r:6 * r + 6]), 0.03)
    i = S["slide down"]                                                                   # 2xx
    t = mt.slide_ticks(214, 4, 4)
    for r in (0, 3):
        near(i, f"row {r}", f_in(i, *row(r)), mt.tick_hz(t[6 * r:6 * r + 6]), 0.03)

    i = S["tone portamento"]                                                              # 3xx: 428 to 214 by 16 per tick, 300 remembers the speed
    t = [428] * 6 + mt.porta_ticks(428, 214, 16, 4)
    for r in range(5):
        near(i, f"row {r}", f_in(i, *row(r)), mt.tick_hz(t[6 * r:6 * r + 6]), 0.03)

    i = S["vibrato"]                                                                      # 4xy: windows of four ticks against the model
    t = mt.vibrato_ticks(214, 4, 8, 8)
    got = [f_in(i, w * 0.08 + 0.005, (w + 1) * 0.08 - 0.005) for w in range(12)]
    for w in range(12):
        near(i, f"window {w}", got[w], mt.tick_hz(t[4 * w:4 * w + 4]), 0.05)
    assert max(got) / min(got) > 1.06, f"{names[i]}: the pitch hardly moves: {[round(g) for g in got]}"

    i = S["tremolo"]                                                                      # 7xy: the peak of every tick against the model
    vols = mt.tremolo_vols(32, 4, 8, 8)
    for k, v in enumerate(vols):
        got = peak_in(i, k * 0.02 + 0.004, k * 0.02 + 0.016)
        assert abs(got - PEAK * v / 64) < 0.02, f"{names[i]}, tick {k}: volume {v} should peak near {PEAK * v / 64:.3f}, found {got:.3f}"

    i = S["volume slide"]                                                                 # Axy: 4 down per tick on ticks 1-5: 64, 44, 24, 4 at the starts of the rows
    for r, v in enumerate((64, 44, 24, 4)):
        got = peak_in(i, r * 0.12 + 0.004, r * 0.12 + 0.016)
        assert abs(got - PEAK * v / 64) < 0.012, f"{names[i]}, row {r}: volume {v} should peak near {PEAK * v / 64:.3f}, found {got:.3f}"

    for name in ("speed", "tempo"):                                                       # rows of 60 ms: sound, silence, sound, silence, sound
        i = S[name]
        r = runs(i)
        assert len(r) == 3, f"{name}: expected three notes, found {r}"
        for n, (s, e) in enumerate(r):
            about(i, f"note {n} starts", s, n * 120, 10)
            about(i, f"note {n} lasts", e - s, 60, 10)

    i = S["note cut"]                                                                     # EC2: two ticks of sound
    r = runs(i)
    assert len(r) == 1, r
    about(i, "length", r[0][1] - r[0][0], 40, 10)

    i = S["note delay"]                                                                   # ED3: after the time mark, the note starts at tick 3
    r = runs(i)
    assert len(r) == 2 and r[0][1] - r[0][0] < 25, f"{names[i]}: expected a time mark and a note, found {r}"
    about(i, "start", r[1][0], 60, 10)
    about(i, "length", r[1][1] - r[1][0], 60, 12)

    i = S["retrigger"]                                                                    # E92: the burst plays again at ticks 2 and 4
    r = runs(i)
    assert len(r) == 3, f"{names[i]}: expected three bursts, found {r}"
    for n, (s, e) in enumerate(r):
        about(i, f"burst {n}", s, n * 40, 10)

    i = S["sample offset"]                                                                # 908 skips the 124 ms of silence at the start of the sample
    r = runs(i)
    assert len(r) == 3, f"{names[i]}: expected the offset note, a time mark and the late note, found {r}"
    assert r[0][0] < 10 and r[0][1] - r[0][0] > 100, f"{names[i]}: the note with an offset should sound at once: {r}"
    assert r[1][1] - r[1][0] < 30, f"{names[i]}: the time mark is a short burst: {r}"
    about(i, "the note without the offset starts after the mark", r[2][0] - r[1][0], 124, 12)

    i = S["pattern loop"]                                                                  # E60 / E62: rows 0 and 1, three times in all
    for r_ in range(6):
        near(i, f"row {r_}", f_in(i, *row(r_)), mt.PAL / (214 if r_ % 2 == 0 else 107), 0.02)

    i = S["finetune"]                                                                     # E54 (+4) and E5C (-4): periods 208 and 220 on the table
    near(i, "row 0", f_in(i, *row(0)), mt.PAL / 208, 0.012)
    near(i, "row 1", f_in(i, *row(1)), mt.PAL / 220, 0.012)

    i = S["fine slides"]                                                                  # E14: 214 - 4; E28: + 8
    near(i, "row 0", f_in(i, *row(0)), mt.PAL / 210, 0.012)
    near(i, "row 1", f_in(i, *row(1)), mt.PAL / 218, 0.012)

    i = S["position jump"]                                                                # B11 skips the pattern that would play 518 Hz for a long time
    near(i, "note", f_in(i, *row(0)), mt.PAL / 214, 0.015)
    about(i, "length", runs(i)[-1][1], 120, 15)
    i = S["the end"]                                                                      # ... and lands on the last one, which ends the song
    near(i, "note", f_in(i, *row(0)), mt.PAL / 160, 0.015)
    about(i, "length", runs(i)[-1][1], 120, 15)


@test
def audio_jukebox():
    """Reimu's Jukebox: plays the first song through the sound card, shows its title, pauses, skips, seeks and sets the volume."""
    from guitools import Gui
    import guitext
    ink = (0x8E, 0x0B, 0x21)                                  # the colour of the "Now playing" line (the theme's dark accent)
    region = (290, 285, 380, 40)

    with Qemu(cmdline="text", name="t-audio-jukebox", mem="768M", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        put_file(vm, "/tmp/test.mod", mt.make_module())      # for the end of the test
        vm.send("\n")                                         # a fresh prompt: start_desktop() waits for one
        g = Gui(vm)
        g.start_desktop()

        def on_screen(text, timeout=90):
            end = time.time() + timeout
            while time.time() < end:
                if guitext.find_text(vm.screen_ppm(), text, ink, region):
                    return True
                time.sleep(1.0)
            vm.screenshot(BUILD / "t-audio-jukebox-fail.png")
            return False

        vm.send("volume 100; jukebox --trace -p &\n")
        vm.expect(r"jukebox: ready with 5 tracks", timeout=180)
        vm.expect(r"jukebox: playing Bamboo Lantern Walk", timeout=60)
        assert on_screen("Now playing: Bamboo Lantern Walk"), "the title of the song never appeared in the window"
        vm.screenshot(BUILD / "t-audio-jukebox.png")
        time.sleep(3)

        vm.key("spc")                                         # pause
        vm.expect(r"jukebox: paused at (\d+\.\d)", timeout=60)
        assert on_screen("Loaded: Bamboo Lantern Walk"), "the window should say the song is loaded, not playing"
        vm.key("n")                                           # next song
        vm.expect(r"jukebox: playing Kappa Workshop Rush", timeout=60)
        assert on_screen("Now playing: Kappa Workshop Rush")
        time.sleep(2)

        g.click(496, 343)                                     # the middle of the seek bar: about 40 s into the 80 s song
        m = vm.expect(r"jukebox: seek (\d+)\.\d", timeout=60)
        assert 30 <= int(m.group(1)) <= 50, f"seek went to {m.group(1)} s"
        g.click(525, 388)                                     # a quarter of the way along the volume bar
        m = vm.expect(r"jukebox: volume (\d+)", timeout=60)
        assert 15 <= int(m.group(1)) <= 35, f"volume went to {m.group(1)}"
        time.sleep(2)
        vm.key("m")
        vm.expect(r"jukebox: muted", timeout=60)
        vm.key("q", ctrl=True)
        time.sleep(1)

        vm.send("volume unmute; volume 100; jukebox --trace -p /tmp/test.mod &\n")      # a ProTracker module in the same window
        vm.expect(r"jukebox: ready with 6 tracks", timeout=180)
        vm.expect(r"jukebox: playing Test Module", timeout=60)
        vm.expect(r"jukebox: playing Bamboo Lantern Walk", timeout=120)                 # the module is over: the list goes on
        vm.key("q", ctrl=True)
        time.sleep(1)
        rate, x = capture(vm)
    assert at.peak(x) > 0.02, "nothing was played"
    segs = at.segments(x, rate, threshold=0.004, merge_ms=400)
    loud = sum((b - a) for a, b in segs) / rate
    assert loud > 4, f"only {loud:.1f} s of music reached the sound card"
    longest = max(segs, key=lambda s: s[1] - s[0])
    seg = x[longest[0]:longest[1]]
    jumps = float(abs(seg[1:] - seg[:-1]).max())
    assert jumps < 0.6, f"a click of {jumps:.2f} in the music"
    a = segs[-1][0]                                                                       # the module, then the song that follows it
    f = at.dominant_freq(x[a + int(0.06 * rate):a + int(0.42 * rate)], rate)
    assert abs(f - 258.97) < 4, f"the module's first notes are 259 Hz, heard {f:.1f}"
    f = at.dominant_freq(x[a + int(0.54 * rate):a + int(0.90 * rate)], rate)
    assert abs(f - 517.9) < 8, f"the module's next notes are 518 Hz, heard {f:.1f}"


@test
def audio_system_sounds():
    """System sounds honour the switch: silent with 'nosounds', on with HAKUREI_SOUNDS=on, off with =off; beep -e forces one."""
    with Qemu(cmdline="text", name="t-audio-snd-off", audio="hda") as vm:                 # the harness adds "nosounds"
        vm.expect(test_shell.PROMPT, timeout=90)
        sh(vm, "volume 100")
        out, rc = sh(vm, "beep -E dialog; echo rc=$?")
        assert "rc=0" in out, out
        out, rc = sh(vm, "sleep 1; HAKUREI_SOUNDS=on beep -E dialog; echo rc=$?")        # the environment wins over the command line
        assert "rc=0" in out, out
        out, rc = sh(vm, "beep -E nonsense; echo rc=$?")
        assert "rc=2" in out and "no system sound called" in out, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.003, merge_ms=300)
    assert len(segs) == 1, f"only the HAKUREI_SOUNDS=on bell should be audible, found {segs}"
    f = at.dominant_freq(at.trimmed(x[segs[0][0]:segs[0][1]], rate, ms=20), rate)
    assert abs(f - 1318.5) < 15, f"the dialog bell is an E6 (1318 Hz), heard {f:.1f}"

    with Qemu(cmdline="text", name="t-audio-snd-on", audio="hda", sounds=True) as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        sh(vm, "volume 100")
        sh(vm, "beep -E error")                                                          # on by default when nothing says otherwise
        sh(vm, "sleep 1; HAKUREI_SOUNDS=off beep -E dialog")                             # silent
        sh(vm, "sleep 1; HAKUREI_SOUNDS=off beep -e shutdown")                           # -e plays whatever the switch says
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.003, merge_ms=300)
    assert len(segs) == 2, f"expected the error buzz and the forced shutdown tune, found {segs}"
    low = at.dominant_freq(at.trimmed(x[segs[0][0]:segs[0][1]], rate, ms=20), rate)
    assert 150 < low < 240, f"the error sound is two low notes (220 and 165 Hz), heard {low:.1f}"
    tune = x[segs[1][0]:segs[1][1]]
    assert at.band_power(tune, rate, 523.25, 25) > 0.1 and (segs[1][1] - segs[1][0]) / rate > 0.7, "the shutdown tune ends on C5"


@test
def audio_desktop_sounds():
    """The desktop plays the login chime when it opens, and the login screen buzzes at a wrong password."""
    with Qemu(cmdline="text", name="t-audio-desktop", mem="768M", audio="hda", sounds=True) as vm:
        from guitools import Gui
        g = Gui(vm)
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("volume 100\n")                               # start_desktop() waits for the prompt that follows
        g.start_desktop()
        time.sleep(6)
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.003, merge_ms=300)
    assert segs, "no login chime"
    chime = x[segs[0][0]:segs[0][1]]
    assert 0.6 < (len(chime) / rate) < 3.0, f"the chime lasts about a second, not {len(chime) / rate:.1f} s"
    assert at.band_power(chime, rate, 1046.5, 25) > 0.12, "the login chime ends on a high C"

    with Qemu(cmdline="login", name="t-audio-greeter", mem="768M", audio="hda", sounds=True) as vm:
        vm.expect(r"opening the login screen", timeout=120)
        time.sleep(8)
        vm.type("wrong\n")
        vm.expect(r"reimu was turned away at the gate", timeout=90)
        time.sleep(4)
        rate, x = capture(vm)
    segs = at.segments(x, rate, threshold=0.003, merge_ms=300)
    assert segs, "the login screen made no sound for a wrong password"
    low = at.dominant_freq(at.trimmed(x[segs[-1][0]:segs[-1][1]], rate, ms=20), rate)
    assert 150 < low < 240, f"the error sound is two low notes, heard {low:.1f} Hz"


@test
def audio_panel_volume():
    """The speaker in the panel: click it for the slider, drag the slider and the master volume follows; Mute mutes."""
    from guitools import Gui
    with Qemu(cmdline="text", name="t-audio-panel", mem="768M", audio="hda") as vm:
        g = Gui(vm)
        g.start_desktop()
        out = g.sh("volume 60")
        for attempt in range(3):                              # on a busy host the pointer can lag behind: try again from a clean start
            g.click(500, 300)                                 # closes a popup that an earlier attempt left open
            g.click(919, 750)                                 # the speaker in the tray
            time.sleep(1.5)
            vm.screenshot(BUILD / "t-audio-panel.png")
            g.click(869, 692)                                 # a quarter of the way along the slider in the popup
            time.sleep(1.5)
            out = g.sh("volume")
            m = re.search(r"Master volume: (\d+)%", out)
            if m and 15 <= int(m.group(1)) <= 35:
                break
        assert m and 15 <= int(m.group(1)) <= 35, f"the slider should have set about 25%, volume says: {out!r}"
        g.click(862, 715)                                     # Mute
        time.sleep(1.5)
        out = g.sh("volume")
        assert "(muted)" in out, out
        g.click(862, 715)                                     # Unmute
        time.sleep(1.5)
        out = g.sh("volume")
        assert "muted" not in out and "Master volume" in out, out


@test
def audio_ac97_tone():
    """AC'97 fallback: a card without HDA works through the same /dev/audio, at the right pitch and level."""
    with Qemu(cmdline="text", name="t-audio-ac97", audio="ac97") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "cat /proc/audio")
        assert rc == 0 and "AC'97" in out, out
        out, rc = sh(vm, "volume 100 && beep -f 523 -l 1500 -a 50")
        assert rc == 0, out
        out, rc = sh(vm, "sleep 1; beep -r 22050 -c 1 -b 8 -f 880 -l 1200 -a 40")
        assert rc == 0, out
        rate, x = capture(vm)
    segs = at.segments(x, rate, merge_ms=400)
    assert len(segs) == 2, f"expected two sounds, found {segs}"
    for (a, b), want, level in zip(segs, (523, 880), (0.5, 0.4)):
        tone = at.trimmed(x[a:b], rate)
        f = at.dominant_freq(tone, rate)
        assert abs(f - want) < want * 0.01 + 2, f"expected {want} Hz, heard {f:.1f} Hz"
        assert abs(at.peak(tone) - level) < 0.07, f"{want} Hz: peak {at.peak(tone):.2f}, expected {level}"
        assert float(abs(tone[1:] - tone[:-1]).max()) < 0.2, "the output has a jump in it"
