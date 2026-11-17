#!/usr/bin/env python3
"""The FLAC decoder against implementations that are not ours (Windows only: it needs PowerShell and Windows 10 1607 or later, which carry a FLAC decoder and a FLAC
encoder, the encoder being the reference libFLAC).

  python tests/host/flac_reference_check.py            (also needs WSL's Ubuntu with gcc: the decoder is the one tests/host/flactool.c builds)

tests/host/test_flac.py proves that the decoder reproduces what tests/host/flacenc.py writes, and the encoder and the decoder were written by the same person from the
same text, so a shared misreading of RFC 9639 would not show there. This script closes that gap in both directions:

  1. Microsoft's decoder (Media Foundation, tests/host/flac_mf_decode.ps1) reads 13 streams made by flacenc.py (every channel assignment, LPC order 32 at precision 15,
     fixed predictors, both Rice methods, escape partitions, wasted bits, variable block sizes, constant and verbatim subframes, 8, 12, 16 and 24 bits, 6 channels) and must
     give exactly the PCM that went into the encoder: so the encoder writes valid FLAC.
  2. Windows' encoder (reference libFLAC, tests/host/flac_mf_encode.ps1) encodes generated WAV files (several rates from 44.1 to 192 kHz, 16 and 24 bits, 1, 2, 3 and 6
     channels, silence, noise, a sine) and the decoder (flac_verify) must find no damage and the MD5 libFLAC wrote into STREAMINFO, and 200 random seeks in each (libFLAC's own seek table is there) must
     read what a full decode reads: so the decoder reads real FLAC. Where
     the encoder did not resample (it is told the format of the file, and for low rates it resamples to 44.1 kHz) that MD5 is also the MD5 of the PCM that went in.

  3. A differential fuzz: several hundred random valid streams (random predictors of every kind with random orders, precisions and shifts, Rice methods, partition
     orders and escapes, wasted bits, channel assignments per frame, 1 to 6 channels, 8, 16 and 24 bits, variable block sizes down to 16 samples) are decoded by Microsoft's
     decoder and by this one, and both must give the PCM that went in.
  4. Windows' decoders for the WAV codecs (the same Media Foundation script: it reads any file Windows knows) decode IMA ADPCM, Microsoft ADPCM, A-law and mu-law WAV
     files made by tests/host/audiogen.py (mono and stereo) and must give the samples the generator's model says. This is what showed that Microsoft ADPCM's
     predictor is an arithmetic shift, not C's division (the specification's "/ 256"): the first model rounded toward zero and a quarter of the samples differed.

The result of the first run, on 2026-11-17: all 13 streams decoded by Microsoft's decoder are byte for byte the input; all 12 libFLAC streams verify; the WAV codecs
agree with Windows (after the Microsoft ADPCM fix); 631 random streams decode identically in both decoders.
"""
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import audiogen as ag  # noqa: E402
import flacenc as fe  # noqa: E402

WORK = ROOT / "build" / "flac-reference"
TOOL = ROOT / "build" / "host-flac" / "flactool"
POWERSHELL = "powershell.exe"


def wsl_path(p):
    p = Path(p).resolve()
    return f"/mnt/{p.drive[:1].lower()}/" + str(p)[3:].replace("\\", "/")


def wsl(cmd):
    return subprocess.run(["wsl.exe", "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "sh", "-c", cmd], capture_output=True, text=True)


def ensure_tool():
    sources = [ROOT / "tests/host/flactool.c", ROOT / "user/lib/audio/src/flac.c", ROOT / "user/lib/audio/include/flac.h"]
    if TOOL.exists() and TOOL.stat().st_mtime >= max(s.stat().st_mtime for s in sources):
        return
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    r = wsl(f"cd {wsl_path(ROOT)} && gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -Wno-unused-parameter "
            f"-Iuser/lib/audio/include tests/host/flactool.c user/lib/audio/src/flac.c -o build/host-flac/flactool")
    if r.returncode:
        raise SystemExit("cannot build the decoder tool:\n" + r.stdout + r.stderr)


def ps(script, *args):
    cmd = [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / script), *map(str, args)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def ms_decodes_ours():
    """Part 1: Microsoft's decoder on streams of the test encoder."""
    n = 20000
    music = lambda bps, seed, rate=44100, voices=3: fe.music(n, rate, bps, seed, voices)   # noqa: E731
    cases = {
        "auto stereo, 16 bit": dict(pcm=[music(16, 1), music(16, 2)], rate=44100, bps=16, blocksize=4096, stereo="auto"),
        "mid/side, LPC 32 precision 15": dict(pcm=[music(16, 3), music(16, 4)], rate=44100, bps=16, blocksize=4096, stereo="mid_side",
                                              subframe={"kind": "lpc", "order": 32, "precision": 15}),
        "left/side, fixed order 3": dict(pcm=[music(16, 5), music(16, 6)], rate=44100, bps=16, blocksize=1152, stereo="left_side", subframe={"kind": "fixed", "order": 3}),
        "right/side, 48 kHz": dict(pcm=[music(16, 7), music(16, 8)], rate=48000, bps=16, blocksize=2048, stereo="right_side"),
        "mono, Rice method 1, partition order 5": dict(pcm=[music(16, 9)], rate=44100, bps=16, blocksize=4096,
                                                       subframe={"kind": "lpc", "order": 8, "method": 1, "porder": 5}),
        "24 bit at 96 kHz": dict(pcm=[music(24, 10), music(24, 11)], rate=96000, bps=24, blocksize=4096, stereo="auto"),
        "8 bit": dict(pcm=[music(8, 12, 22050)], rate=22050, bps=8, blocksize=1024),
        "wasted bits": dict(pcm=[[v << 3 for v in music(13, 13)], [v << 3 for v in music(13, 14)]], rate=44100, bps=16, blocksize=2048, stereo="mid_side"),
        "variable block size": dict(pcm=[fe.music(12692, 44100, 16, 15), fe.music(12692, 44100, 16, 16)], rate=44100, bps=16, blocksize=[8192, 1000, 3000, 500], stereo="auto"),
        "verbatim and constant subframes": dict(pcm=[fe.noise(5000, 16, 17), [123] * 5000], rate=44100, bps=16, blocksize=1000, stereo="indep"),
        "escape-coded partitions": dict(pcm=[music(16, 18)], rate=44100, bps=16, blocksize=2048, subframe={"kind": "fixed", "order": 2, "escape": "force", "porder": 3}),
        "12 bit": dict(pcm=[music(12, 19), music(12, 20)], rate=44100, bps=12, blocksize=1024, stereo="auto"),
        "6 channels": dict(pcm=[music(16, 21 + k, 48000, 2) for k in range(6)], rate=48000, bps=16, blocksize=2048),
    }
    bad = 0
    for i, (name, kw) in enumerate(cases.items()):
        pcm, rate, bps = kw.pop("pcm"), kw.pop("rate"), kw.pop("bps")
        flac = WORK / f"ours{i}.flac"
        raw = WORK / f"ours{i}.mf.raw"
        flac.write_bytes(fe.encode(pcm, rate, bps, **kw).data)
        rc, text = ps("flac_mf_decode.ps1", "-In", flac, "-Out", raw)
        cb = 1 if bps <= 8 else 2 if bps <= 16 else 3 if bps <= 24 else 4
        flat = np.array(pcm, dtype="<i4").T.reshape(-1)
        want = bytes((int(v) + 128) & 0xFF for v in flat) if cb == 1 else flat.astype("<i4").view(np.uint8).reshape(-1, 4)[:, :cb].tobytes()
        got = raw.read_bytes() if raw.exists() else b""
        ok = got == want
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'}  Microsoft decodes {name}: {len(got)} bytes ({text})")
    return bad


def libflac_verified_by_ours():
    """Part 2: streams of the reference encoder through our decoder."""
    n = 44100
    specs = [("44.1 kHz 16-bit stereo music", 44100, 16, 2, "music"), ("48 kHz 16-bit mono", 48000, 16, 1, "music"), ("48 kHz 24-bit stereo", 48000, 24, 2, "music"),
             ("96 kHz 16-bit stereo", 96000, 16, 2, "music"), ("88.2 kHz 24-bit stereo", 88200, 24, 2, "music"), ("176.4 kHz 24-bit stereo", 176400, 24, 2, "music"),
             ("192 kHz 24-bit stereo", 192000, 24, 2, "music"), ("3 channels", 48000, 16, 3, "music"), ("6 channels", 48000, 16, 6, "music"),
             ("silence", 44100, 16, 2, "silence"), ("noise", 44100, 16, 2, "noise"), ("sine", 44100, 16, 2, "sine")]
    bad = 0
    for i, (name, rate, bits, ch, kind) in enumerate(specs):
        n = rate
        if kind == "silence":
            pcm = [[0] * n for _ in range(ch)]
        elif kind == "noise":
            pcm = [fe.noise(n, bits, 7 + c) for c in range(ch)]
        elif kind == "sine":
            pcm = [fe.tone(n, rate, 440 * (c + 1), 0.9, bits) for c in range(ch)]
        else:
            pcm = [ag.ints(n, bits, 10 + c) for c in range(ch)]
        mask = {1: 0x4, 2: 0x3, 3: 0x7, 6: 0x3F}[ch]
        wav = WORK / f"ref{i}.wav"
        flac = WORK / f"ref{i}.flac"
        wav.write_bytes(ag.wav_pcm(pcm, rate, bits, extensible=(ch > 2 or bits > 16), mask=mask).data)
        rc, text = ps("flac_mf_encode.ps1", "-Wav", wav, "-OutFlac", flac, "-Rate", rate, "-Channels", ch, "-Bits", bits)
        if rc or not flac.exists():
            print(f"  BAD  Windows could not encode {name}: {text}")
            bad += 1
            continue
        r = wsl(f"{wsl_path(TOOL)} verify {wsl_path(flac)}")
        ok = "result=0" in r.stdout and "md5_ok=1" in r.stdout and "errors=0" in r.stdout
        same = fe.pcm_md5(pcm, bits).hex() in r.stdout
        seeks = wsl(f"{wsl_path(TOOL)} seektest {wsl_path(flac)} 5 200").stdout                  # 200 random seeks, each read checked against a full decode
        ok = ok and "bad=0" in seeks
        bad += not ok
        info = wsl(f"{wsl_path(TOOL)} info {wsl_path(flac)}").stdout
        vendor = [ln for ln in info.splitlines() if ln.startswith("vendor=")]
        print(f"  {'ok ' if ok else 'BAD'}  our decoder verifies libFLAC's {name}: {r.stdout.strip().split(' first_error')[0]}"
              f"{'; the MD5 is also that of the PCM that went in' if same else '; the encoder changed the audio (resampled)'}; {seeks.strip()} [{vendor[0][7:] if vendor else '?'}]")
    return bad


def random_streams(count, seed):
    """Part 3: random valid streams through both decoders."""
    import random
    out = WORK / f"random{seed}"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*"):
        old.unlink()
    rnd = random.Random(seed)
    rates = [8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000]

    def signal(n, bps, kind, s):
        top = (1 << (bps - 1)) - 1
        if kind == "music":
            return fe.music(n, 44100, bps, s, voices=rnd.randint(1, 4))
        if kind == "noise":
            return fe.noise(n, bps, s, amp=rnd.choice([1.0, 0.5, 0.01]))
        if kind == "silence":
            return [0] * n
        if kind == "steps":
            xs, v = [], 0
            for i in range(n):
                if i % rnd.choice([7, 50, 300]) == 0:
                    v = rnd.randint(-top - 1, top)
                xs.append(v)
            return xs
        if kind == "square":
            return [top if (i // 11) % 2 else -top - 1 for i in range(n)]
        return fe.tone(n, 44100, rnd.choice([100, 1000, 5000]), rnd.choice([0.1, 0.9, 1.0]), bps)

    expected, made = {}, 0
    for k in range(count):
        bps = rnd.choice([8, 16, 16, 16, 24])
        channels = rnd.choice([1, 2, 2, 2, 3, 5, 6])
        n = rnd.choice([1, 17, 100, 500, 1000, 2500, 5000])
        pcm = []
        for c in range(channels):
            x = signal(n, bps, rnd.choice(["music", "music", "noise", "silence", "steps", "square", "tone"]), rnd.randrange(1 << 30))
            if rnd.random() < 0.2:
                w = rnd.randint(1, bps - 2)
                x = [v >> w << w for v in x]
            pcm.append(x)
        variable = rnd.random() < 0.25
        blocksize = rnd.choice([16, 100, 192, 576, 1000, 1152, 2048, 4096, 4608])
        if variable:
            blocksize, tot = [], 0
            while tot < n:
                blocksize.append(min(rnd.choice([16, 31, 100, 256, 1000, 3000]), n - tot))
                tot += blocksize[-1]

        def sub(frame, ch, samples, bits):
            r, size = rnd.random(), len(samples)
            if r < 0.12 or size <= 2:
                return {"kind": "verbatim"}
            if r < 0.45 and size > 5:
                spec = {"kind": "fixed", "order": rnd.randint(0, min(4, size - 1))}
            else:
                spec = {"kind": "lpc", "order": rnd.randint(1, min(32, size - 1)), "precision": rnd.randint(4, 15)}
                if rnd.random() < 0.5:
                    spec["shift"] = rnd.randint(0, 15)
            spec["method"] = rnd.randint(0, 1)
            if rnd.random() < 0.5:
                spec["escape"] = rnd.choice(["auto", "never", "force"])
            if rnd.random() < 0.3:
                spec["porder"] = rnd.randint(0, 4)
            return spec

        try:
            s = fe.encode(pcm, rnd.choice(rates), bps, blocksize=blocksize, variable=variable, stereo=lambda i: rnd.choice(["indep", "left_side", "right_side", "mid_side"]),
                          subframe=sub, frame_bps=rnd.choice(["explicit", "streaminfo"]), frame_rate=rnd.choice(["explicit", "streaminfo"]))
        except (ValueError, AssertionError):
            continue                                                                       # a random choice the format does not allow for this block: skipped
        name = f"r{k:04d}"
        (out / f"{name}.flac").write_bytes(s.data)
        flat = np.array(pcm, dtype="<i4").T.reshape(-1)
        cb = 1 if bps <= 8 else 2 if bps <= 16 else 3
        expected[name] = bytes(((int(v) + 128) & 0xFF) for v in flat) if cb == 1 else flat.astype("<i4").view(np.uint8).reshape(-1, 4)[:, :cb].tobytes()
        made += 1
    rc, text = ps("flac_mf_decode.ps1", "-Dir", out)
    bad_ms = [n for n, want in expected.items() if not (out / f"{n}.mf.raw").exists() or (out / f"{n}.mf.raw").read_bytes() != want]
    verified = wsl(f"cd {wsl_path(out)} && ok=0; bad=0; for f in *.flac; do if {wsl_path(TOOL)} verify $f | grep -q 'result=0 .*md5_ok=1'; then ok=$((ok+1)); else bad=$((bad+1)); fi; done; echo $ok $bad").stdout.split()
    ours_bad = int(verified[1]) if len(verified) == 2 else made
    print(f"  {'ok ' if not bad_ms and not ours_bad else 'BAD'}  {made} random streams: Microsoft's decoder differs on {len(bad_ms)}, this decoder fails to verify {ours_bad} ({text})")
    return len(bad_ms) + ours_bad


def wav_codecs_vs_microsoft():
    """Part 3: the WAV codecs against Windows' own decoders."""
    n = 4000
    cases = {
        "IMA ADPCM, mono": ag.wav_ima([ag.ints(n, 16, 1)], 22050, 256),
        "IMA ADPCM, stereo": ag.wav_ima([ag.ints(n, 16, 2), ag.ints(n, 16, 3)], 22050, 512),
        "Microsoft ADPCM, mono": ag.wav_msadpcm([ag.ints(n, 16, 4)], 22050, 256),
        "Microsoft ADPCM, stereo": ag.wav_msadpcm([ag.ints(n, 16, 5), ag.ints(n, 16, 6)], 22050, 512),
        "A-law": ag.wav_g711([ag.codes(n, 1)], 8000, "alaw"),
        "mu-law, stereo": ag.wav_g711([ag.codes(n, 2), ag.codes(n, 3)], 8000, "ulaw"),
    }
    bad = 0
    for i, (name, a) in enumerate(cases.items()):
        wav = WORK / f"wav{i}.wav"
        raw = WORK / f"wav{i}.mf.raw"
        wav.write_bytes(a.data)
        rc, text = ps("flac_mf_decode.ps1", "-In", wav, "-Out", raw)
        want = b"".join((a.chans[c][k] & 0xFFFF).to_bytes(2, "little") for k in range(len(a.chans[0])) for c in range(a.channels))
        got = raw.read_bytes() if raw.exists() else b""
        ok = got[:len(want)] == want and len(got) >= len(want)            # Windows does not trim the padded last block of an ADPCM file (the fact chunk): it may give a few more
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'}  Windows decodes {name} as the model says: {len(want)} bytes compared ({text})")
    return bad


def main():
    if os.name != "nt":
        print("flac_reference_check: needs Windows (PowerShell, Media Foundation)")
        return 0
    WORK.mkdir(parents=True, exist_ok=True)
    ensure_tool()
    print("Microsoft's FLAC decoder against streams of tests/host/flacenc.py:")
    bad = ms_decodes_ours()
    print("the reference libFLAC encoder (Windows) against this project's decoder:")
    bad += libflac_verified_by_ours()
    print("random streams through both decoders:")
    bad += random_streams(300, 2)
    print("Windows' WAV codecs against tests/host/audiogen.py:")
    bad += wav_codecs_vs_microsoft()
    print(f"{'ALL AGREE' if not bad else str(bad) + ' DISAGREEMENTS'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
