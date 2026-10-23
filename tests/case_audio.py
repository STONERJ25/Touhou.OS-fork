"""Sound: the Intel HDA and AC'97 drivers, the mixer behind /dev/audio, and the tools on top of it.

QEMU's sound card plays into a WAV file (Qemu(audio=...)); the tests measure that file on the host with numpy. The guest
clock can be several times slower than wall time, so nothing here depends on how long a sound lasts: tests wait for the
shell to come back, stop the VM, and look at the loud stretches of the recording."""
import re
import time

import audiotools as at
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
    with Qemu(cmdline="text hda.imm hda.posbuf", name="t-audio-fallback", audio="hda-duplex") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = sh(vm, "cat /proc/audio")
        assert "commands by the immediate registers" in out and "position from the position buffer" in out, out
        assert "line out" in out and "output:   pin" in out, out
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


@test
def audio_jukebox():
    """Reimu's Jukebox: plays the first song through the sound card, shows its title, pauses, skips, seeks and sets the volume."""
    from guitools import Gui
    import guitext
    ink = (0x8E, 0x0B, 0x21)                                  # the colour of the "Now playing" line (the theme's dark accent)
    region = (290, 285, 380, 40)

    with Qemu(cmdline="text", name="t-audio-jukebox", mem="768M", audio="hda") as vm:
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
        rate, x = capture(vm)
    assert at.peak(x) > 0.02, "nothing was played"
    segs = at.segments(x, rate, threshold=0.004, merge_ms=400)
    loud = sum((b - a) for a, b in segs) / rate
    assert loud > 4, f"only {loud:.1f} s of music reached the sound card"
    longest = max(segs, key=lambda s: s[1] - s[0])
    seg = x[longest[0]:longest[1]]
    jumps = float(abs(seg[1:] - seg[:-1]).max())
    assert jumps < 0.6, f"a click of {jumps:.2f} in the music"


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
