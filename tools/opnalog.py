#!/usr/bin/env python3
"""Reads the register-write log of `pc98run --opna-log` (cycle,bank,reg,val) and says what music a driver programmed: the notes of each channel.

  python tools/opnalog.py build/pc98/th01_opna_log2.csv --mhz 16 --from 8 --to 30
  python tools/opnalog.py LOG.csv --mhz 16 --channel fm1 --list 40        (the first 40 notes of FM channel 1)

For every FM channel (fm1-fm6) a note runs from a key-on (register 28h with any slot bit set) to the next key-off (all slot bits clear) of the same channel;
its pitch is the F-number and block as last written (A4h-A6h latched, A0h-A2h applying them, the way the chip does it) with f = fs * ((fnum << block) >> 1) /
2^20, fs = 7987200 / 144 = 55466.67 Hz (the -86 board's YM2608). For the SSG (ssg1-ssg3) a note is a channel whose tone is enabled in the mixer (register 07h
bit n clear) with a level above zero (08h-0Ah, bit 4 means "envelope"), and f = 7987200 / (128 * period). Times are machine seconds (cycles / CPU clock).
Rhythm key-ons (register 10h) are counted by drum. This is a summary of the log, not a transcription of the song: a pitch bend inside a held note is one note.
"""
import argparse
import csv
import math
from collections import defaultdict

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
PHIM = 7987200.0
FS = PHIM / 144.0


def name(hz):
    if hz <= 0:
        return "-"
    n = 12 * math.log2(hz / 440.0) + 69
    k = round(n)
    return "%s%d%+.0f" % (NOTES[k % 12], k // 12 - 1, (n - k) * 100)


def parse(log, mhz):
    """Reads a `pc98run --opna-log` CSV: returns (notes, drums, total, last_t): notes = {channel name: [(start s, end s, Hz)]}, drums = {name: key-ons},
    the number of register writes and the time of the last one (machine seconds, mhz = the CPU clock the run used)."""
    hz_cpu = mhz * 1e6
    reg = [[0] * 256, [0] * 256]
    latch = [0] * 6                      # A4h-A6h high part waiting for its low byte, per channel
    fnum = [0] * 6
    block = [0] * 6
    on = {}                              # channel -> (start time, pitch)
    notes = defaultdict(list)            # channel name -> [(t0, t1, hz)]
    drums = defaultdict(int)
    ssg_on = {}
    total = 0
    last_t = 0.0

    def fm_hz(c):
        return FS * ((fnum[c] << block[c]) >> 1) / float(1 << 20)

    def ssg_hz(c):
        tp = reg[0][c * 2] | (reg[0][c * 2 + 1] & 15) << 8
        return PHIM / (128.0 * (tp or 1))

    def ssg_update(c, t):
        audible = not (reg[0][7] >> c) & 1 and (reg[0][8 + c] & 31) != 0
        if audible and c not in ssg_on:
            ssg_on[c] = (t, ssg_hz(c))
        elif not audible and c in ssg_on:
            t0, hz = ssg_on.pop(c)
            notes["ssg%d" % (c + 1)].append((t0, t, hz))

    with open(log, newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        t = int(row["cycle"]) / hz_cpu
        last_t = t
        bank, r, v = int(row["bank"]), int(row["reg"], 16), int(row["val"], 16)
        reg[bank][r] = v
        total += 1
        if bank == 0 and r == 0x28:
            c = (v & 3) + (3 if v & 4 else 0)
            if v & 0xF0:
                if c not in on:
                    on[c] = (t, fm_hz(c))
            elif c in on:
                t0, hz = on.pop(c)
                notes["fm%d" % (c + 1)].append((t0, t, hz))
        elif r in (0xA4, 0xA5, 0xA6):
            latch[(r - 0xA4) + 3 * bank] = v
        elif r in (0xA0, 0xA1, 0xA2):
            c = (r - 0xA0) + 3 * bank
            fnum[c] = (latch[c] & 7) << 8 | v
            block[c] = (latch[c] >> 3) & 7
        elif bank == 0 and r in range(0, 14):
            if r == 7 or 8 <= r <= 10 or r < 6:
                for c in range(3):
                    ssg_update(c, t)
        elif bank == 0 and r == 0x10 and not v & 0x80:
            for d, dn in enumerate(("bass", "snare", "cymbal", "hihat", "tom", "rim")):
                if v & (1 << d):
                    drums[dn] += 1
    for c, (t0, hz) in on.items():
        notes["fm%d" % (c + 1)].append((t0, last_t, hz))
    for c, (t0, hz) in ssg_on.items():
        notes["ssg%d" % (c + 1)].append((t0, last_t, hz))
    return notes, drums, total, last_t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--mhz", type=float, default=16.0, help="CPU clock of the run (the cycle column is CPU clocks)")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--channel", default="", help="only this channel (fm1-fm6, ssg1-ssg3)")
    ap.add_argument("--list", type=int, default=12, help="notes to list per channel")
    a = ap.parse_args()
    notes, drums, total, last_t = parse(a.log, a.mhz)
    print("%d register writes, last at %.2f s" % (total, last_t))
    for ch in sorted(notes):
        if a.channel and a.channel != ch:
            continue
        ns = [n for n in notes[ch] if a.t0 <= n[0] <= a.t1]
        if not ns:
            continue
        gaps = sorted(ns[i + 1][0] - ns[i][0] for i in range(len(ns) - 1))
        print("%s: %d notes, %.2f-%.2f s, shortest onset gap %.3f s, median %.3f s" % (
            ch, len(ns), ns[0][0], ns[-1][1], gaps[0] if gaps else 0, gaps[len(gaps) // 2] if gaps else 0))
        print("   " + " ".join("%s@%.2f" % (name(h), s) for s, e, h in ns[:a.list]))
    if drums:
        print("rhythm key-ons: " + ", ".join("%s %d" % kv for kv in sorted(drums.items())))


if __name__ == "__main__":
    main()
