"""Sound: the Intel HDA and AC'97 drivers, the mixer behind /dev/audio, and the tools on top of it.

QEMU's sound card plays into a WAV file (Qemu(audio=...)); the tests measure that file on the host with numpy. The guest
clock can be several times slower than wall time, so nothing here depends on how long a sound lasts: tests wait for the
shell to come back, stop the VM, and look at the loud stretches of the recording."""
import re

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
