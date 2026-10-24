"""Building ProTracker modules for the sound tests, and working out what their effects are documented to do.

A module is made from scratch (ModBuilder), so that every number a test checks can be worked out by hand: the samples are
a 64-byte sine wave (period 214 plays it at 259 Hz: PAL / 214), a short decaying burst and a long sample that starts with
silence. At speed 6 and tempo 125 a tick lasts 20 ms and a row 120 ms. The models below follow the ProTracker description of
each effect one tick at a time; they are written down again here, in Python, on purpose: the C player is what is tested."""
import math
import struct

import numpy as np

PAL = 3546894.6 / 64                       # Hz times period, for a wave of 64 bytes per cycle
SINE = bytes(round(100 * math.sin(2 * math.pi * i / 64)) & 0xFF for i in range(64))


class ModBuilder:
    """Samples, patterns of 64 rows by 4 channels and an order list; build() gives the bytes of a 'M.K.' module."""

    def __init__(self, title):
        self.title, self.samples, self.patterns, self.order = title, [], [], []

    def sample(self, data, volume=64, loop=False, finetune=0, name=None):
        """Adds an 8-bit sample and returns its number (1 to 31). A looped sample repeats over its whole length."""
        assert len(data) % 2 == 0 and len(self.samples) < 31
        self.samples.append((name or f"sample {len(self.samples) + 1}", bytes(data), volume, loop, finetune))
        return len(self.samples)

    def pattern(self):
        """Appends an empty pattern to the order list and returns put(row, channel, sample, period, effect, param)."""
        rows = [[(0, 0, 0, 0)] * 4 for _ in range(64)]
        self.patterns.append(rows)
        self.order.append(len(self.patterns) - 1)

        def put(row, ch=0, sample=0, period=0, effect=0, param=0):
            rows[row][ch] = (sample, period, effect, param)
        return put

    def build(self):
        data = self.title.encode().ljust(20, b"\0")
        for i in range(31):
            if i < len(self.samples):
                name, d, vol, loop, ft = self.samples[i]
                words = len(d) // 2
                data += name.encode().ljust(22, b"\0") + struct.pack(">HBBHH", words, ft & 15, vol, 0, words if loop else 1)
            else:
                data += bytes(22) + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
        data += bytes([len(self.order), 127]) + bytes(self.order) + bytes(128 - len(self.order)) + b"M.K."
        for rows in self.patterns:
            for row in rows:
                for sample, period, effect, param in row:
                    data += bytes([(sample & 0xF0) | (period >> 8), period & 0xFF, ((sample & 0x0F) << 4) | effect, param])
        for _name, d, *_rest in self.samples:
            data += d
        return data


def make_module():
    """The small module of audio_mod_module. One looped sine sample.

    Pattern 0: rows 0-3 a note on channel 1 (left), rows 4-7 the same note an octave up, rows 8-11 the first note on
    channel 2 (right), row 12 jumps on. Pattern 1: rows 0-7 the note with an arpeggio of 0, 3 and 7 semitones; row 8 ends
    the song and, with a pattern delay, lasts two rows: 23 rows, 2.76 s in all."""
    b = ModBuilder("Test Module")
    s = b.sample(SINE, 64, loop=True, name="sine")
    p = b.pattern()
    p(0, 0, s, 214)
    p(4, 0, s, 107)
    p(8, 0, effect=0xC)                                                         # set volume 0
    p(8, 1, s, 214)
    p(12, 1, effect=0xC)
    p(12, 3, effect=0xD)                                                        # pattern break: on to pattern 1
    p = b.pattern()
    p(0, 0, s, 214, 0, 0x37)                                                    # arpeggio 0, 3, 7
    for row in range(1, 8):
        p(row, 0, effect=0, param=0x37)
    p(8, 0, effect=0xC)
    p(8, 1, effect=0xE, param=0xE1)                                             # pattern delay: this row twice
    p(8, 3, effect=0xD)                                                         # and then the end of the song
    return b.build()


FX_SECTIONS = ("slide up", "slide down", "tone portamento", "vibrato", "tremolo", "volume slide", "speed", "tempo",
               "note cut", "note delay", "retrigger", "sample offset", "pattern loop", "finetune", "fine slides",
               "position jump", "the end")


def make_effects_module():
    """One short section for each effect, in the order of FX_SECTIONS, each followed by two silent rows (240 ms) so that the
    sections can be told apart in a recording. Channel 1 plays the effect; channel 3 carries the pattern break, and
    channel 2 plays a short burst as a time mark where a section needs one."""
    b = ModBuilder("Effects")
    burst = bytes(round(100 * math.exp(-i / 60) * math.sin(2 * math.pi * i / 32)) & 0xFF for i in range(192))
    lead = bytes(2048) + SINE * 32                                              # 4096 bytes: 124 ms of silence at period 214, then a tone
    s1 = b.sample(SINE, 64, loop=True)
    s2 = b.sample(burst, 64)
    s3 = b.sample(lead, 64)
    s4 = b.sample(SINE, 32, loop=True)

    def end(p, row):
        p(row, 0, effect=0xC)                                                   # silence, twice, and the pattern break
        p(row + 1, 0, effect=0xC)
        p(row + 1, 3, effect=0xD)

    p = b.pattern()                                                             # 0: slide up 1xx
    p(0, 0, s1, 428, 1, 4)
    for r in (1, 2, 3):
        p(r, 0, effect=1, param=4)
    end(p, 4)

    p = b.pattern()                                                             # 1: slide down 2xx
    p(0, 0, s1, 214, 2, 4)
    for r in (1, 2, 3):
        p(r, 0, effect=2, param=4)
    end(p, 4)

    p = b.pattern()                                                             # 2: tone portamento 3xx, with memory
    p(0, 0, s1, 428)
    p(1, 0, s1, 214, 3, 0x10)
    for r in (2, 3, 4):
        p(r, 0, effect=3, param=0)
    end(p, 5)

    p = b.pattern()                                                             # 3: vibrato 4xy, speed 4 depth 8, with memory
    p(0, 0, s1, 214, 4, 0x48)
    for r in range(1, 8):
        p(r, 0, effect=4, param=0)
    end(p, 8)

    p = b.pattern()                                                             # 4: tremolo 7xy around volume 32
    p(0, 0, s4, 214, 7, 0x48)
    for r in range(1, 8):
        p(r, 0, effect=7, param=0)
    end(p, 8)

    p = b.pattern()                                                             # 5: volume slide Axy
    p(0, 0, s1, 214, 0xA, 4)
    for r in (1, 2, 3):
        p(r, 0, effect=0xA, param=4)
    end(p, 4)

    p = b.pattern()                                                             # 6: speed Fxx (< 32): rows of 3 ticks
    p(0, 0, s1, 214, 0xF, 3)
    p(1, 0, effect=0xC)
    p(2, 0, s1, 214)
    p(3, 0, effect=0xC)
    p(4, 0, s1, 214)
    p(5, 0, effect=0xC)
    p(6, 0, effect=0xF, param=6)
    end(p, 7)

    p = b.pattern()                                                             # 7: tempo Fxx (>= 32): ticks of 10 ms
    p(0, 0, s1, 214, 0xF, 0xFA)
    p(1, 0, effect=0xC)
    p(2, 0, s1, 214)
    p(3, 0, effect=0xC)
    p(4, 0, s1, 214)
    p(5, 0, effect=0xC)
    p(5, 3, effect=0xF, param=0x7D)
    end(p, 6)

    p = b.pattern()                                                             # 8: note cut EC2
    p(0, 0, s1, 214, 0xE, 0xC2)
    end(p, 1)

    p = b.pattern()                                                             # 9: note delay ED3, after a time mark
    p(0, 2, s2, 214)
    p(0, 0, s1, 214, 0xE, 0xD3)
    end(p, 1)

    p = b.pattern()                                                             # 10: retrigger E92
    p(0, 0, s2, 214, 0xE, 0x92)
    end(p, 1)

    p = b.pattern()                                                             # 11: sample offset 908, with a note without it after
    p(0, 2, s2, 214)
    p(0, 0, s3, 214, 9, 0x08)
    p(1, 0, effect=0xC)
    p(2, 2, s2, 214)
    p(2, 0, s3, 214)
    end(p, 4)

    p = b.pattern()                                                             # 12: pattern loop E60 / E62
    p(0, 0, s1, 214, 0xE, 0x60)
    p(1, 0, s1, 107, 0xE, 0x62)
    end(p, 2)

    p = b.pattern()                                                             # 13: finetune E54 (+4) and E5C (-4)
    p(0, 0, s1, 214, 0xE, 0x54)
    p(1, 0, s1, 214, 0xE, 0x5C)
    end(p, 2)

    p = b.pattern()                                                             # 14: fine slides E14 and E28
    p(0, 0, s1, 214, 0xE, 0x14)
    p(1, 0, effect=0xE, param=0x28)
    end(p, 2)

    p = b.pattern()                                                             # 15: position jump B11 over the next pattern
    p(0, 0, s1, 214)
    p(1, 0, effect=0xC)
    p(2, 0, effect=0xC)
    p(2, 3, effect=0xB, param=17)
    p = b.pattern()                                                             # 16: never played; a different pitch, for a long time
    p(0, 0, s1, 107)
    p = b.pattern()                                                             # 17: the end: a jump back to a row that was played
    p(0, 0, s1, 160)
    p(1, 0, effect=0xC)
    p(1, 3, effect=0xB, param=0)
    return b.build()


# ---------------------------------------------------------------------------------------------- what the effects do

def tick_hz(periods):
    """The average frequency of the 64-byte sine while it plays at these periods, one for each tick."""
    return sum(PAL / p for p in periods) / len(periods)


def slide_ticks(start, step, rows, speed=6):
    """1xx and 2xx: the period moves by step on every tick but the first of a row (113 to 856)."""
    p, out = start, []
    for _ in range(rows):
        for t in range(speed):
            if t:
                p = min(856, max(113, p + step))
            out.append(p)
    return out


def porta_ticks(start, target, step, rows, speed=6):
    """3xx: the period moves towards the target by step on every tick but the first of a row, and stops there."""
    p, out = start, []
    for _ in range(rows):
        for t in range(speed):
            if t and p != target:
                p = max(target, p - step) if p > target else min(target, p + step)
            out.append(p)
    return out


def vib_delta(pos, depth):
    """The sine table of vibrato and tremolo: 255 sin, times depth over 128, negative in the second half of 64 positions."""
    amp = int(255 * math.sin(math.pi * (pos & 31) / 32))
    v = amp * depth // 128
    return -v if pos & 32 else v


def vibrato_ticks(base, speed, depth, rows, ticks=6):
    """4xy: no change on the first tick of a row; then the sine at a position that moves on by speed after each use."""
    pos, out = 0, []
    for _ in range(rows):
        for t in range(ticks):
            if t == 0:
                out.append(base)
            else:
                out.append(base + vib_delta(pos, depth))
                pos += speed
    return out


def tremolo_vols(vol, speed, depth, rows, ticks=6):
    """7xy: like vibrato, but the sine is added to the volume, twice as strongly (depth over 64), within 0 to 64."""
    pos, out = 0, []
    for _ in range(rows):
        for t in range(ticks):
            if t == 0:
                out.append(vol)
            else:
                out.append(min(64, max(0, vol + 2 * vib_delta(pos, depth))))
                pos += speed
    return out


def mean_freq(sig, rate):
    """The average frequency of an oscillating signal: rising zero crossings, placed between samples, per second."""
    neg = sig < 0
    idx = np.where(neg[:-1] & ~neg[1:])[0]
    if len(idx) < 3:
        return 0.0
    pos = idx + (-sig[idx]) / (sig[idx + 1] - sig[idx])
    return float((len(pos) - 1) * rate / (pos[-1] - pos[0]))
