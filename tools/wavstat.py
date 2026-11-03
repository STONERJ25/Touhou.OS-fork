#!/usr/bin/env python3
"""Numbers for a WAV file, for the people (and agents) who cannot listen: duration, peak, RMS, where it is silent, and what notes are in it.

  python tools/wavstat.py build/pc98/th01_start.wav
  python tools/wavstat.py FILE.wav --window 1.0 --peaks 4      (a line per second with its loudest frequencies)
  python tools/wavstat.py FILE.wav --from 12 --to 20 --notes   (only that part, with the note names of the strongest tones)

Reads 16-bit PCM (mono or stereo; stereo is averaged for the spectrum and reported per side for the level). Uses numpy for the FFT. The spectrum
of a window is a Hann-windowed FFT; "peaks" are the strongest local maxima that are not within 3 bins of a stronger one, reported as Hz and
as the nearest equal-tempered note (A4 = 440 Hz) with the deviation in cents. It says what is in the file, not whether it sounds right.
"""
import argparse
import math
import struct
import sys
import wave

import numpy as np

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(hz):
    if hz <= 0:
        return "-"
    n = 12 * math.log2(hz / 440.0) + 69
    k = round(n)
    cents = (n - k) * 100
    return "%s%d%+.0fc" % (NOTES[k % 12], k // 12 - 1, cents)


def load(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        sys.exit("wavstat: only 16-bit PCM")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    return data.reshape(-1, ch), rate


def peaks(x, rate, count):
    """The `count` strongest spectral peaks of x (Hann window): [(hz, amplitude)], amplitude in sample units."""
    if len(x) < 64:
        return []
    win = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * win)) * 2.0 / win.sum()
    freqs = np.fft.rfftfreq(len(x), 1.0 / rate)
    order = np.argsort(spec)[::-1]
    out, taken = [], []
    for i in order:
        if freqs[i] < 20 or any(abs(i - j) <= 3 for j in taken):
            continue
        if i > 0 and i < len(spec) - 1 and not (spec[i] >= spec[i - 1] and spec[i] >= spec[i + 1]):
            continue
        # parabolic interpolation of the peak position
        a, b, c = (spec[i - 1], spec[i], spec[i + 1]) if 0 < i < len(spec) - 1 else (0, spec[i], 0)
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
        out.append((float((i + d) * rate / len(x)), float(b)))
        taken.append(i)
        if len(out) == count:
            break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--window", type=float, default=0.0, help="seconds per line of the time table (0: none)")
    ap.add_argument("--peaks", type=int, default=3, help="strongest frequencies per window / for the whole file")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=0.0)
    ap.add_argument("--notes", action="store_true", help="name the notes")
    a = ap.parse_args()
    data, rate = load(a.wav)
    n0 = int(a.t0 * rate)
    n1 = int(a.t1 * rate) if a.t1 else len(data)
    data = data[n0:n1]
    mono = data.mean(axis=1)
    dur = len(data) / rate
    print("%s: %.3f s, %d Hz, %d channel(s), %d frames" % (a.wav, dur, rate, data.shape[1], len(data)))
    if not len(data):
        return
    peak = np.abs(data).max(axis=0)
    rms = np.sqrt((data ** 2).mean(axis=0))
    print("peak %s of 32767 (%.1f dBFS), rms %s (%.1f dBFS)" % (
        "/".join(str(int(p)) for p in peak), 20 * math.log10(max(peak.max(), 1) / 32768.0),
        "/".join("%.0f" % r for r in rms), 20 * math.log10(max(rms.max(), 1e-9) / 32768.0)))
    clipped = int((np.abs(data) >= 32767).sum())
    print("clipped samples: %d; DC offset %.1f" % (clipped, mono.mean()))
    # silence: 50 ms blocks below -60 dBFS
    blk = max(1, int(rate * 0.05))
    levels = [np.sqrt((mono[i:i + blk] ** 2).mean()) for i in range(0, len(mono) - blk + 1, blk)]
    quiet = [lv < 32768 * 10 ** (-60 / 20) for lv in levels]
    runs, start = [], None
    for i, q in enumerate(quiet + [False]):
        if q and start is None:
            start = i
        if not q and start is not None:
            if (i - start) * 0.05 >= 0.25:
                runs.append((a.t0 + start * 0.05, a.t0 + i * 0.05))
            start = None
    print("silent stretches (>= 0.25 s below -60 dBFS): %s" % (", ".join("%.2f-%.2f s" % r for r in runs) if runs else "none"))
    first = next((i for i, q in enumerate(quiet) if not q), None)
    print("first sound at %.2f s" % (a.t0 + first * 0.05) if first is not None else "no sound at all")
    pk = peaks(mono[:1 << 20], rate, a.peaks * 2)
    if len(mono) > 0:
        print("whole file, strongest frequencies: " + ", ".join(
            "%.1f Hz%s (%.0f)" % (f, (" " + note_name(f)) if a.notes else "", amp) for f, amp in pk))
    if a.window > 0:
        step = int(a.window * rate)
        print("  t(s)   peak    rms  loudest frequencies")
        for i in range(0, len(mono) - step // 2, step):
            seg = mono[i:i + step]
            p = peaks(seg, rate, a.peaks)
            print("%6.1f %6d %6.0f  %s" % (a.t0 + i / rate, np.abs(seg).max(), np.sqrt((seg ** 2).mean()),
                                           "  ".join("%.0f Hz%s" % (f, (" " + note_name(f)) if a.notes else "") for f, amp in p if amp > 30)))


if __name__ == "__main__":
    main()
