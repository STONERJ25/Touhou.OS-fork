"""Helpers for the sound tests: read the WAV file QEMU's audio backend captured and measure it.

The sound card in the guest plays into a WAV file (see Qemu(audio=...)); the file is complete once the VM has stopped.
The guest clock can run much slower than the wall clock on a busy host, so tests never rely on exact durations: they look
for the loud stretches of the capture and measure those."""
import struct
from pathlib import Path

import numpy as np


def load(path):
    """Returns (rate, samples): samples is a float32 array of shape (frames, channels) in -1..1. Tolerates a header whose
    sizes were never filled in (QEMU killed before it could finish the file)."""
    raw = Path(path).read_bytes()
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise AssertionError(f"{path}: not a WAV file ({len(raw)} bytes)")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack_from("<I", raw, pos + 4)[0]
        body = pos + 8
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", raw, body)
        elif tag == b"data":
            end = body + size
            if size == 0 or size == 0xFFFFFFFF or end > len(raw):
                end = len(raw)
            data = raw[body:end]
            break
        pos = body + size + (size & 1)
    assert fmt and data is not None, f"{path}: no fmt/data chunk"
    _tag, channels, rate, _brate, _align, bits = fmt
    assert bits == 16, f"expected 16-bit capture, got {bits}"
    n = len(data) // 2 // channels * channels
    x = np.frombuffer(data[:n * 2], dtype="<i2").astype(np.float32) / 32768.0
    return rate, x.reshape(-1, channels)


def mono(x):
    return x.mean(axis=1) if x.ndim == 2 else x


def segments(x, rate, threshold=0.01, merge_ms=150, min_ms=100):
    """Loud stretches of a mono signal as (start, end) sample indexes. Quiet gaps shorter than merge_ms do not split one."""
    m = mono(x)
    win = max(1, rate // 200)                         # 5 ms envelope
    n = len(m) // win
    if n == 0:
        return []
    env = np.abs(m[:n * win]).reshape(n, win).max(axis=1)
    loud = env > threshold
    out, start, last = [], None, None
    for i, on in enumerate(loud):
        if on:
            if start is None:
                start = i
            last = i
        elif start is not None and (i - last) * win * 1000 / rate > merge_ms:
            out.append((start * win, (last + 1) * win))
            start = None
    if start is not None:
        out.append((start * win, (last + 1) * win))
    return [(a, b) for a, b in out if (b - a) * 1000 / rate >= min_ms]


def dominant_freq(x, rate):
    """Frequency of the strongest spectral peak of a mono signal (Hann window, parabolic interpolation)."""
    m = mono(x).astype(np.float64)
    if len(m) < 256:
        return 0.0
    m = m * np.hanning(len(m))
    spec = np.abs(np.fft.rfft(m, n=max(len(m), rate)))
    k = int(np.argmax(spec[1:])) + 1
    if 1 <= k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0
        k = k + d
    return float(k * rate / max(len(m), rate))


def band_power(x, rate, freq, width=30):
    """Signal power within +-width Hz of freq, as a fraction of the total power."""
    m = mono(x).astype(np.float64)
    m = m * np.hanning(len(m))
    spec = np.abs(np.fft.rfft(m)) ** 2
    freqs = np.fft.rfftfreq(len(m), 1.0 / rate)
    total = spec[1:].sum() + 1e-12
    return float(spec[(freqs > freq - width) & (freqs < freq + width)].sum() / total)


def peak(x):
    return float(np.abs(x).max()) if len(x) else 0.0


def trimmed(x, rate, ms=40):
    """The signal without its first and last few milliseconds (fades, start-up)."""
    k = rate * ms // 1000
    return x[k:-k] if len(x) > 2 * k else x
