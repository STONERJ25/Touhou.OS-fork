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
