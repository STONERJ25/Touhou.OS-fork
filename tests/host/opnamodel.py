"""A second, independent model of the YM2203/YM2608 for tests/host/test_opna.py.

Written from the chips' descriptions (the application manuals' register map, formulas and tables; docs/PC98_SOUND.md),
not from user/lib/pc98/src/opna.c: the tables come from Python's math module, the generators are written in the most
literal form (closed formulas where the hardware is a counter, the manual's attack formula with ~att, plain lists for
the algorithm diagrams), and it runs sample by sample. It shares no code with the C model; where both must make the
same choice that the manuals leave open (marked "unverified" in the docs), the choice is spelled out here again.
"""
import math

# ------------------------------------------------------------------ tables from their definitions

def _round(v):
    return int(v + 0.5)


LOGSIN = [_round(-math.log2(math.sin((2 * i + 1) * math.pi / 1024)) * 256) for i in range(256)]
EXP = [_round(1024 * 2 ** ((255 - i) / 256)) for i in range(256)]
SSG_DAC = [0] + [_round(8191 * 2 ** ((i - 31) / 4)) for i in range(1, 32)]
RHYTHM_GAIN = [_round(32767 * 2 ** (-i / 8)) for i in range(96)]
PM_CENTS_X10 = [0, 34, 67, 100, 140, 200, 400, 800]
PM_RATIO = [[_round(65536 * (2 ** (PM_CENTS_X10[p] * q / (7 * 12000)) - 1)) for q in range(8)] for p in range(8)]

# The manuals' detune table in phase increment units, DT 1..3 (DT 5..7 subtract the same).
DETUNE = [
    [0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8],
    [1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16],
    [2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22],
]

LFO_DIVIDER = [109, 78, 72, 68, 63, 45, 9, 6]
AM_SHIFT = [8, 3, 1, 0]

# The algorithm diagrams: who modulates each operator (S1 = 0, S2 = 1, S3 = 2, S4 = 3) and who is heard.
MODULATORS = [
    {1: [0], 2: [1], 3: [2]},
    {2: [0, 1], 3: [2]},
    {2: [1], 3: [0, 2]},
    {1: [0], 3: [1, 2]},
    {1: [0], 3: [2]},
    {1: [0], 2: [0], 3: [0]},
    {1: [0]},
    {},
]
CARRIERS = [[3], [3], [3], [3], [1, 3], [1, 2, 3], [1, 2, 3], [0, 1, 2, 3]]

EG_PATTERN_LOW = [[0, 1, 0, 1, 0, 1, 0, 1], [0, 1, 0, 1, 1, 1, 0, 1], [0, 1, 1, 1, 0, 1, 1, 1], [0, 1, 1, 1, 1, 1, 1, 1]]
EG_PATTERN_HIGH = [[1, 1, 1, 1, 1, 1, 1, 1], [1, 1, 1, 2, 1, 1, 1, 2], [1, 2, 1, 2, 1, 2, 1, 2], [1, 2, 2, 2, 1, 2, 2, 2]]


def op_lookup(phase20, mod, att):
    """One operator output: 10-bit phase (+ modulation) -> log-sine; + 4 * attenuation; -> exponent; sign."""
    p = ((phase20 >> 10) + mod) % 1024
    quarter = p % 256
    if p & 256:
        quarter = 255 - quarter
    a = LOGSIN[quarter] + 4 * att
    v = (EXP[a % 256] * 4) >> (a // 256)
    return -v if p & 512 else v


def keycode(fnum, block):
    f11, f10, f9, f8 = (fnum >> 10) & 1, (fnum >> 9) & 1, (fnum >> 8) & 1, (fnum >> 7) & 1
    n3 = (f11 and (f10 or f9 or f8)) or (not f11 and f10 and f9 and f8)
    return 4 * block + 2 * f11 + (1 if n3 else 0)


def phase_inc(fnum, block, dt, mul, pm=0):
    base = fnum << block
    if pm > 0:
        base += (base * pm) >> 16
    elif pm < 0:
        base -= (base * -pm) >> 16
    f = base >> 1
    if dt & 3:
        d = DETUNE[(dt & 3) - 1][keycode(fnum, block)]
        f = f - d if dt & 4 else f + d
    f %= 1 << 17
    return (f * mul) % (1 << 20) if mul else f >> 1


def eg_increment(rate, counter):
    if rate < 2:
        return 0
    if rate < 48:
        shift = 11 - rate // 4
        if counter % (1 << shift):
            return 0
        return EG_PATTERN_LOW[rate % 4][(counter >> shift) % 8]
    if rate >= 60:
        return 8
    return EG_PATTERN_HIGH[rate % 4][counter % 8] << (rate // 4 - 12)


# ------------------------------------------------------------------ SSG

def ssg_env_level(shape, steps):
    """The envelope level (0..31) after `steps` steps since the shape register was written."""
    up = (shape >> 2) & 1
    if steps < 32:
        return steps if up else 31 - steps
    if not shape & 8:
        return 0
    if shape & 1:
        return 31 if ((shape >> 2) ^ (shape >> 1)) & 1 else 0
    ramp, pos = divmod(steps, 32)
    if (shape & 2) and (ramp & 1):
        up ^= 1
    return pos if up else 31 - pos


def ssg_frames(chip, regs, n):
    """The SSG's sum of channels for each of n FM samples (the mean of the SSG ticks inside each sample), every
    register written at time 0. Closed forms: after tick k a tone is at (k // TP) & 1, the noise register has shifted
    (k + 1) // (2 NP) times, the envelope has taken k // EP steps."""
    sample_clocks, ssg_clocks = (72, 32) if chip == 2203 else (144, 64)
    r = [regs.get(i, 0) for i in range(16)]
    tp = [max(1, r[2 * i] | (r[2 * i + 1] & 15) << 8) for i in range(3)]
    np_ = max(1, r[6] & 31)
    ep = max(1, r[11] | r[12] << 8)
    lfsr, shifted = 1, 0
    out, done, last = [], 0, 0
    for s in range(1, n + 1):
        upto = s * sample_clocks // ssg_clocks
        vals = []
        for k in range(done + 1, upto + 1):
            want = (k + 1) // (2 * np_)
            while shifted < want:
                bit = (lfsr ^ (lfsr >> 3)) & 1
                lfsr = (lfsr >> 1) | (bit << 16)
                shifted += 1
            env = ssg_env_level(r[13], k // ep)
            total = 0
            for i in range(3):
                tone_off, noise_off = (r[7] >> i) & 1, (r[7] >> (i + 3)) & 1
                tone = (k // tp[i]) & 1
                if (tone or tone_off) and ((lfsr & 1) or noise_off):
                    v = r[8 + i]
                    level = env if v & 16 else (2 * (v & 15) + 1 if v & 15 else 0)
                    total += SSG_DAC[level]
            vals.append(total)
        done = upto
        if vals:
            last = sum(vals) // len(vals)
        out.append(last)
    return out


# ------------------------------------------------------------------ FM

ATTACK, DECAY, SUSTAIN, RELEASE = range(4)


class Operator:
    def __init__(self):
        self.dt = self.mul = self.tl = self.ks = self.ar = self.am = self.dr = self.sr = self.sl = self.rr = 0
        self.key = False
        self.state = RELEASE
        self.att = 1023
        self.phase = 0
        self.out = 0


class Channel:
    def __init__(self, stereo):
        self.ops = [Operator() for _ in range(4)]
        self.fnum = self.block = self.alg = self.fb = self.ams = self.pms = 0
        self.left = self.right = stereo
        self.history = [0, 0]


class Fm:
    """The FM part of either chip: write() registers, sample() -> (left, right) sums of the channels."""

    def __init__(self, chip):
        self.chip = chip
        self.ch = [Channel(chip == 2608) for _ in range(6)]
        self.six = False
        self.latch = self.latch3 = 0
        self.freq3 = [(0, 0), (0, 0), (0, 0)]          # S1, S2, S3 in the special mode
        self.mode3 = 0
        self.lfo_on = False
        self.lfo_freq = self.lfo_cnt = self.lfo_div = 0
        self.eg_div = 0
        self.eg_counter = 1

    def channels(self):
        return 6 if self.chip == 2608 and self.six else 3

    def freq(self, c, i):
        if c == 2 and self.mode3 and i < 3:
            return self.freq3[i]
        return self.ch[c].fnum, self.ch[c].block

    def rate(self, op, kc):
        r = {ATTACK: op.ar, DECAY: op.dr, SUSTAIN: op.sr, RELEASE: op.rr * 2 + 1}[op.state]
        return min(63, 2 * r + (kc >> (3 - op.ks))) if r else 0

    def key(self, c, i, on):
        op = self.ch[c].ops[i]
        if on and not op.key:
            op.phase = 0
            op.state = ATTACK
            if self.rate(op, keycode(*self.freq(c, i))) >= 62:
                op.att, op.state = 0, DECAY
        elif not on and op.key:
            op.state = RELEASE
        op.key = on

    def write(self, bank, reg, val):
        if self.chip == 2203 and bank:
            return
        if bank == 0 and reg < 0x30:
            if reg == 0x22 and self.chip == 2608:
                self.lfo_on, self.lfo_freq = bool(val & 8), val & 7
                if not self.lfo_on:
                    self.lfo_cnt = self.lfo_div = 0
            elif reg == 0x27:
                self.mode3 = val >> 6
            elif reg == 0x28:
                c = (val & 3) + (3 if val & 4 else 0)
                if (val & 3) != 3 and c < self.channels():
                    for i in range(4):
                        self.key(c, i, bool(val & (16 << i)))
            elif reg == 0x29 and self.chip == 2608:
                self.six = bool(val & 0x80)
            return
        if reg < 0x30 or (reg & 3) == 3:
            return
        c = (reg & 3) + 3 * bank
        ch = self.ch[c]
        if reg >= 0xA0:
            group = reg & 0xFC
            if group == 0xA0:
                ch.fnum, ch.block = (self.latch & 7) << 8 | val, self.latch >> 3
            elif group == 0xA4:
                self.latch = val & 0x3F
            elif group == 0xA8 and bank == 0:
                self.freq3[{0: 2, 1: 0, 2: 1}[reg & 3]] = ((self.latch3 & 7) << 8 | val, self.latch3 >> 3)
            elif group == 0xAC and bank == 0:
                self.latch3 = val & 0x3F
            elif group == 0xB0:
                ch.alg, ch.fb = val & 7, (val >> 3) & 7
            elif group == 0xB4 and self.chip == 2608:
                ch.left, ch.right, ch.ams, ch.pms = bool(val & 0x80), bool(val & 0x40), (val >> 4) & 3, val & 7
            return
        op = ch.ops[{0: 0, 1: 2, 2: 1, 3: 3}[(reg >> 2) & 3]]
        kind = reg >> 4
        if kind == 3:
            op.dt, op.mul = (val >> 4) & 7, val & 15
        elif kind == 4:
            op.tl = val & 0x7F
        elif kind == 5:
            op.ks, op.ar = val >> 6, val & 31
        elif kind == 6:
            op.am, op.dr = (val >> 7 if self.chip == 2608 else 0), val & 31
        elif kind == 7:
            op.sr = val & 31
        elif kind == 8:
            op.sl, op.rr = val >> 4, val & 15

    def eg(self, op, kc):
        rate = self.rate(op, kc)
        inc = eg_increment(rate, self.eg_counter)
        if op.state == ATTACK:
            if rate >= 62:
                op.att = 0
            elif inc:
                op.att += (~op.att * inc) >> 4             # the manuals' form: attenuation += (~attenuation * inc) / 16
            if op.att <= 0:
                op.att, op.state = 0, DECAY
        else:
            op.att += inc
            if op.state == DECAY and op.att >= (31 * 32 if op.sl == 15 else op.sl * 32):
                op.state = SUSTAIN
        op.att = min(op.att, 1023)

    def lfo_am(self):
        if not self.lfo_on:
            return 0
        c = self.lfo_cnt
        return 2 * (c if c < 64 else 127 - c)

    def lfo_pm(self, pms):
        if not self.lfo_on or not pms:
            return 0
        step = self.lfo_cnt >> 2
        q = 7 - (step & 7) if step & 8 else step & 7
        return -PM_RATIO[pms][q] if step & 16 else PM_RATIO[pms][q]

    def channel(self, c, tick, am):
        ch = self.ch[c]
        pm = self.lfo_pm(ch.pms)
        heard = 0
        for i in (0, 2, 1, 3):                          # the chip's order: S1, S3, S2, S4
            op = ch.ops[i]
            fnum, block = self.freq(c, i)
            if tick:
                self.eg(op, keycode(fnum, block))
            if i == 0:
                mod = (ch.history[0] + ch.history[1]) >> (10 - ch.fb) if ch.fb else 0
            else:
                mod = sum(ch.ops[s].out for s in MODULATORS[ch.alg].get(i, [])) >> 1
            att = op.att + 8 * op.tl
            if op.am and self.chip == 2608:
                att += am >> AM_SHIFT[ch.ams]
            v = op_lookup(op.phase, mod, min(att, 1023))
            op.out = v
            if i == 0:
                ch.history = [v, ch.history[0]]
            op.phase = (op.phase + phase_inc(fnum, block, op.dt, op.mul, pm)) % (1 << 20)
            if i in CARRIERS[ch.alg]:
                heard += v
        return max(-8192, min(8191, heard))

    def sample(self):
        if self.lfo_on and self.chip == 2608:
            self.lfo_div += 1
            if self.lfo_div >= LFO_DIVIDER[self.lfo_freq]:
                self.lfo_div = 0
                self.lfo_cnt = (self.lfo_cnt + 1) % 128
        self.eg_div += 1
        tick = self.eg_div == 3
        if tick:
            self.eg_div = 0
            self.eg_counter = self.eg_counter + 1 if self.eg_counter < 4095 else 1
        am = self.lfo_am()
        left = right = 0
        for c in range(self.channels()):
            v = self.channel(c, tick, am)
            if self.chip == 2203:
                left += v
                right += v
            else:
                left += v if self.ch[c].left else 0
                right += v if self.ch[c].right else 0
        clamp = lambda x: max(-32768, min(32767, x))
        return clamp(left), clamp(right)


# ------------------------------------------------------------------ ADPCM-B (delta-T)

ADPCM_MULT = [57, 57, 57, 57, 77, 102, 128, 153]       # the manual's 0.9, 0.9, 0.9, 0.9, 1.2, 1.6, 2.0, 2.4 in 1/64


def adpcm_step(x, step, nib):
    l3, l2, l1 = (nib >> 2) & 1, (nib >> 1) & 1, nib & 1
    d = ((8 * l3 + 4 * l2 + 2 * l1 + 1) * step) >> 3      # (L3 + L2/2 + L1/4 + 1/8) * step
    x = x - d if nib & 8 else x + d
    x = max(-32768, min(32767, x))
    step = max(127, min(24576, (step * ADPCM_MULT[nib & 7]) >> 6))
    return x, step


def adpcm_encode(samples):
    """A greedy encoder: for each target sample the nibble whose decoded value lands nearest. Returns bytes (high
    nibble first)."""
    x, step, nibs = 0, 127, []
    for t in samples:
        best = min(range(16), key=lambda n: abs(adpcm_step(x, step, n)[0] - t))
        x, step = adpcm_step(x, step, best)
        nibs.append(best)
    if len(nibs) % 2:
        nibs.append(0)
    return bytes(nibs[i] << 4 | nibs[i + 1] for i in range(0, len(nibs), 2))


def adpcm_play(data, delta_n, level, n, left=True, right=True, repeat=False):
    """Playback as the chip does it once per FM sample: DELTA-N / 65536 new decoded samples, the output interpolated
    between the last two, times level / 256."""
    nibs = [b >> 4 if k == 0 else b & 15 for b in data for k in (0, 1)]
    pos = idx = 0
    x = prev = 0
    step = 127
    playing = True
    out = []
    for _ in range(n):
        if playing:
            pos += delta_n
            while pos >= 65536 and playing:
                pos -= 65536
                prev = x
                x, step = adpcm_step(x, step, nibs[idx])
                idx += 1
                if idx == len(nibs):
                    if repeat:
                        idx, x, prev, step, pos = 0, 0, 0, 127, 0
                    else:
                        playing = False
        if not playing:
            out.append((0, 0))
            continue
        s = prev + (((x - prev) * pos) >> 16)
        s = (s * level) >> 8
        out.append((s if left else 0, s if right else 0))
    return out


# ------------------------------------------------------------------ rhythm

def rhythm_play(pcm, rate, clock, sample_clocks, total_level, inst_level, n):
    """One drum keyed on at time 0: linear interpolation through the PCM at its rate, the level in 0.75 dB steps."""
    step = ((rate * sample_clocks) << 32) // clock
    pos = 0
    att = (63 - total_level) + (31 - inst_level)
    out = []
    for _ in range(n):
        at = pos >> 32
        if at >= len(pcm):
            out.append(0)
            continue
        s0 = pcm[at]
        s1 = pcm[at + 1] if at + 1 < len(pcm) else 0
        frac = (pos >> 16) & 0xFFFF
        s = s0 + (((s1 - s0) * frac) >> 16)
        pos += step
        out.append((s * RHYTHM_GAIN[att]) >> 15)
    return out


def synthetic_drum(count, period):
    """What opnatool's 'rhythmpcm IDX RATE COUNT PERIOD' supplies: a square of PERIOD samples per half, fading out."""
    return [(1 if ((i // period) & 1) else -1) * (20000 - i * 20000 // count) for i in range(count)]
