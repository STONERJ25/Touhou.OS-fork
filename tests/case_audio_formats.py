"""Sound file formats in the guest: FLAC, WAV in all its kinds (PCM of every size, float, A-law, mu-law, IMA and Microsoft ADPCM, extensible, RF64), AIFF, AIFF-C and Sun .au.

The host makes every test file (tests/host/flacenc.py is a FLAC encoder written from RFC 9639, tests/host/audiogen.py makes the others) and puts them into a copy of
the initrd; the guest's `play -w` renders each to a WAV file and the md5 of that file is compared with the one the host computes from the samples it put in (the
same rate, no resampling in the way: the guest must give back exactly the PCM that made the file, bit for bit), `play -i` and the Jukebox list show what the file says
about itself, `play -s -t -w` seeks, a corrupt file is reported and skipped, and one test plays through the real device path (the HDA card with the capture backend)
and finds the tones at the right pitch. The decoders themselves are checked much harder on the host: tests/host/test_flac.py and tests/host/test_audio_formats.py
(sanitizers, 80000 mutations, Python's wave, aifc, sunau and audioop as independent references); these tests show that what runs in the guest is the same code.
"""
import hashlib
import io
import random
import re
import shutil
import sys
import tarfile
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "host"))
import audiogen as ag  # noqa: E402
import flacenc as fe  # noqa: E402
import audiotools as at  # noqa: E402
import test_shell  # noqa: E402
import case_audio  # noqa: E402
from case_audio import capture, sh, starved_retry  # noqa: E402
from harness import Qemu, BUILD, test, scaled  # noqa: E402

MTIME = 1700000000
CACHE = BUILD / "audiofmt-cache"
MASKS = {1: 0x4, 2: 0x3, 3: 0x7, 4: 0x33, 5: 0x37, 6: 0x3F, 7: 0x70F, 8: 0x63F}


# ------------------------------------------------------------------------------------------------------------------ helpers

def audio_esp(files, name):
    """Copies build/esp to build/<name> with `files` ({path in the guest: bytes}) added to the initrd; returns the path."""
    src = BUILD / "esp"
    esp = BUILD / name
    (esp / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    shutil.copy2(src / "kernel.elf", esp / "kernel.elf")
    with tarfile.open(src / "initrd.img") as old, tarfile.open(esp / "initrd.img", "w", format=tarfile.USTAR_FORMAT) as new:
        for m in old:
            new.addfile(m, old.extractfile(m) if m.isreg() else None)
        made = set()
        for path, data in sorted(files.items()):
            parts = path.split("/")
            for i in range(1, len(parts)):
                d = "/".join(parts[:i])
                if d not in made:
                    made.add(d)
                    ti = tarfile.TarInfo(d)
                    ti.type = tarfile.DIRTYPE
                    ti.mode = 0o755
                    ti.mtime = MTIME
                    new.addfile(ti)
            ti = tarfile.TarInfo(path)
            ti.size = len(data)
            ti.mode = 0o644
            ti.mtime = MTIME
            new.addfile(ti, io.BytesIO(data))
    return esp


def generator_stamp():
    h = hashlib.sha1()
    for f in ("flacenc.py", "audiogen.py"):
        h.update((HERE / "host" / f).read_bytes())
    return h.hexdigest()[:10]


def flac_audio(name, pcm, rate, bps, tags=None, **kw):
    """A FLAC stream made by the host encoder (cached on disk: the pure Python encoder is slow) as an Audio: the stream and what it must decode to."""
    CACHE.mkdir(parents=True, exist_ok=True)
    cache = CACHE / f"{name}-{generator_stamp()}.flac"
    if cache.exists():
        data = cache.read_bytes()
    else:
        data = fe.encode(pcm, rate, bps, **kw).data
        cache.write_bytes(data)
    return ag.Audio(data, [list(c) for c in pcm], rate, bps, "FLAC", "FLAC", MASKS[len(pcm)], dict(tags or {}))


def md5_of(a, **kw):
    return hashlib.md5(ag.render_wav(a, **kw)).hexdigest()


def play_wav(vm, name, extra="", timeout=300, out="/tmp/o.wav"):
    """`play -q -w` of a guest file: (exit status, the text it printed, md5 of the WAV it wrote)."""
    text, _rc = sh(vm, f"rm -f {out}; play -q {extra} -w {out} /audiofmt/{name} 2>&1; echo rc=$?; md5sum {out} 2>&1", timeout=timeout)
    m = re.search(r"rc=(\d+)", text)
    h = re.search(r"([0-9a-f]{32})\s+" + re.escape(out), text)
    return int(m.group(1)) if m else -1, text, h.group(1) if h else None


def check_render(vm, name, a, extra="", expect_md5=None):
    rc, text, h = play_wav(vm, name, extra)
    assert rc == 0, f"{name}: play exited with {rc}: {text}"
    want = expect_md5 or md5_of(a)
    assert h == want, f"{name}: the guest rendered a WAV with md5 {h}, the host's model says {want}\n{text}"


# ------------------------------------------------------------------------------------------------------------------ corpora

def flac_corpus():
    n = 24000
    c = {}
    c["f16s.flac"] = flac_audio("f16s", [fe.music(n, 44100, 16, 1), fe.tone(n, 44100, 440, 0.3, 16)], 44100, 16, blocksize=4096, stereo="auto", seek_every=8192,
                               comments=["TITLE=Moonlit Test", "ARTIST=Reimu", "ALBUM=Gensokyo", "TRACKNUMBER=3", "DATE=2026"],
                               tags={"title": "Moonlit Test", "artist": "Reimu", "album": "Gensokyo", "track": "3", "date": "2026"})
    c["f24m.flac"] = flac_audio("f24m", [fe.music(8000, 96000, 24, 2)], 96000, 24, blocksize=2048, subframe={"kind": "lpc", "order": 12})
    c["f8.flac"] = flac_audio("f8", [fe.music(8000, 8000, 8, 3)], 8000, 8, blocksize=1000)
    c["f12.flac"] = flac_audio("f12", [fe.music(6000, 44100, 12, 4), fe.music(6000, 44100, 12, 5)], 44100, 12, blocksize=1152, stereo="mid_side")
    c["f32.flac"] = flac_audio("f32", [fe.noise(4000, 32, 6), fe.noise(4000, 32, 7)], 48000, 32, blocksize=1000, stereo="mid_side")
    c["f20.flac"] = flac_audio("f20", [fe.music(3000, 44100, 20, 8), fe.music(3000, 44100, 20, 9)], 44100, 20, blocksize=1000, stereo="left_side")
    c["f6.flac"] = flac_audio("f6", [fe.music(5000, 48000, 16, 10 + k, voices=2) for k in range(6)], 48000, 16, blocksize=1024)
    sizes = [1000, 333, 4096, 192, 2000, 576, 2500, 1500]
    tot = sum(sizes)
    c["fvar.flac"] = flac_audio("fvar", [fe.music(tot, 22050, 16, 11), fe.music(tot, 22050, 16, 12)], 22050, 16, blocksize=sizes, stereo="auto")
    c["f1.flac"] = flac_audio("f1", [[-12345]], 44100, 16)
    c["fempty.flac"] = flac_audio("fempty", [[], []], 44100, 16)
    c["fpic.flac"] = flac_audio("fpic", [fe.music(5000, 44100, 16, 13)], 44100, 16, blocksize=2048, picture_bytes=300000, padding=20000,
                               comments=["TITLE=Big Cover", "artist=Case"], tags={"title": "Big Cover", "artist": "Case"})
    c["fid3.flac"] = flac_audio("fid3", [fe.music(3000, 44100, 16, 14)], 44100, 16, blocksize=1000,
                               id3v2=b"ID3\x04\x00\x00" + bytes([0, 0, 0, 40]) + bytes(40))
    return c


def wav_corpus():
    n = 3000
    c = {}
    tags = {"INAM": "Wave Title", "IART": "Wave Artist", "IPRD": "Wave Album", "ITRK": "4", "ICRD": "2026"}
    c["pcm8m.wav"] = ag.wav_pcm([ag.ints(n, 8, 1)], 11025, 8)
    c["pcm16s.wav"] = ag.wav_pcm([ag.ints(n, 16, 2), ag.ints(n, 16, 3)], 44100, 16, list_info=tags)
    c["pcm24s.wav"] = ag.wav_pcm([ag.ints(n, 24, 4), ag.ints(n, 24, 5)], 48000, 24)
    c["pcm32m.wav"] = ag.wav_pcm([ag.ints(n, 32, 6)], 96000, 32)
    c["pcm12in16.wav"] = ag.wav_pcm([ag.ints(n, 12, 7), ag.ints(n, 12, 8)], 44100, 12, container=16, extensible=True, mask=0x3)
    c["pcm20in24.wav"] = ag.wav_pcm([ag.ints(n, 20, 9)], 44100, 20, container=24, extensible=True, mask=0x4)
    rnd = random.Random(3)
    c["flt32.wav"] = ag.wav_float([[rnd.uniform(-1.1, 1.1) for _ in range(n)] for _ in range(2)], 44100, 32)
    c["flt64.wav"] = ag.wav_float([[rnd.uniform(-1, 1) for _ in range(n)]], 48000, 64)
    c["alaw.wav"] = ag.wav_g711([ag.codes(n, 1), ag.codes(n, 2)], 8000, "alaw")
    c["ulaw.wav"] = ag.wav_g711([ag.codes(n, 3)], 8000, "ulaw")
    c["ima.wav"] = ag.wav_ima([ag.ints(n, 16, 10), ag.ints(n, 16, 11)], 22050, 512)
    c["msadpcm.wav"] = ag.wav_msadpcm([ag.ints(n, 16, 12)], 22050, 256)
    c["ext6.wav"] = ag.wav_pcm([ag.ints(1000, 24, 20 + k) for k in range(6)], 48000, 24, extensible=True, mask=0x3F)
    c["rf64.wav"] = ag.wav_pcm([ag.ints(n, 16, 13), ag.ints(n, 16, 14)], 44100, 16, kind=b"RF64", ds64=True)
    c["bw64.wav"] = ag.wav_pcm([ag.ints(1000, 24, 15)] * 6, 48000, 24, extensible=True, mask=0x3F, kind=b"BW64", ds64=True)
    c["zero.wav"] = ag.wav_pcm([ag.ints(n, 16, 16), ag.ints(n, 16, 17)], 44100, 16, data_size="zero")
    c["ffff.wav"] = ag.wav_pcm([ag.ints(n, 16, 18)], 44100, 16, data_size="ffff")
    c["odd.wav"] = ag.wav_pcm([ag.ints(501, 8, 19)], 11025, 8, before_data=ag._chunk(b"JUNK", b"abcde"), after_data=ag._chunk(b"id3 ", b"xyz"),
                              list_info={"INAM": "After the data"})
    c["pcm16s.aif"] = ag.aiff([ag.ints(n, 16, 21), ag.ints(n, 16, 22)], 44100, 16, tags={"title": "Aiff Title", "artist": "Aiff Artist"})
    c["pcm12.aif"] = ag.aiff([ag.ints(n, 12, 23)], 22050, 12)
    c["sowt.aifc"] = ag.aiff([ag.ints(n, 16, 24), ag.ints(n, 16, 25)], 44100, 16, comp="sowt")
    c["fl32.aifc"] = ag.aiff([[rnd.uniform(-1, 1) for _ in range(n)]], 44100, 32, comp="fl32")
    c["ulaw.aifc"] = ag.aiff([ag.codes(n, 4)], 8000, 16, comp="ulaw")
    c["pcm24.aif"] = ag.aiff([ag.ints(n, 24, 26), ag.ints(n, 24, 27)], 48000, 24)
    c["pcm16s.au"] = ag.au([ag.ints(n, 16, 31), ag.ints(n, 16, 32)], 16000, 3)
    c["pcm8.au"] = ag.au([ag.ints(n, 8, 33)], 8000, 2)
    c["mulaw.au"] = ag.au([ag.codes(n, 5)], 8000, 1)
    c["alaw.au"] = ag.au([ag.codes(n, 6), ag.codes(n, 7)], 8000, 27)
    c["f32.au"] = ag.au([[rnd.uniform(-1, 1) for _ in range(n)]], 44100, 6)
    c["noext"] = ag.wav_pcm([ag.ints(1000, 16, 41)], 22050, 16)                      # found by its first bytes, not by its name
    c["wrong.mp3"] = ag.aiff([ag.ints(1000, 16, 42)], 22050, 16)
    return c


# ----------------------------------------------------------------------------------------------------------------- the tests

@test
def audio_fmt_flac():
    """FLAC in the guest: 4 to 32 bits, mono to six channels, variable block size, a seek table, a big picture, an ID3v2 tag, one sample, none at all: `play -w` gives
    back the PCM that made each stream, bit for bit; `play -i` shows the tags and `play -V` finds the MD5 in order; `play -s -t -w` seeks to the right sample."""
    corpus = flac_corpus()
    esp = audio_esp({"audiofmt/" + k: a.data for k, a in corpus.items()}, "esp-audiofmt-flac")
    with Qemu(cmdline="text", name="t-audiofmt-flac", esp=esp, mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        for name, a in corpus.items():
            check_render(vm, name, a)
            out, rc = sh(vm, f"play -V /audiofmt/{name}")
            assert rc == 0 and "OK, MD5 matches" in out, f"{name}: {out}"
        # what play -i says
        out, rc = sh(vm, "play -i /audiofmt/f16s.flac")
        assert rc == 0, out
        assert "FLAC, 44100 Hz, 16-bit, stereo, 0.5 s" in out, out
        for line in ("24000 frames", "title: Moonlit Test", "artist: Reimu", "album: Gensokyo", "track: 3", "date: 2026"):
            assert line in out, f"{line!r} missing from:\n{out}"
        out, rc = sh(vm, "play -i /audiofmt/f6.flac")
        assert "FLAC, 48000 Hz, 16-bit, 6 channels" in out and "speakers 0x3f" in out, out
        out, rc = sh(vm, "play -i /audiofmt/f12.flac; play -i /audiofmt/fempty.flac")
        assert "FLAC, 44100 Hz, 12-bit, stereo" in out and "length unknown" in out, out
        out, rc = sh(vm, "play -i /audiofmt/fpic.flac; play -i /audiofmt/fid3.flac")
        assert "title: Big Cover" in out and "artist: Case" in out and "FLAC, 44100 Hz, 16-bit, mono, 0.1 s" in out, out
        # seeking: a part of the stream, by the seek table (f16s) and by searching (fvar has none)
        a = corpus["f16s.flac"]
        check_render(vm, "f16s.flac", a, "-s 0.25 -t 0.125", md5_of(a, start=11025, count=5512))
        check_render(vm, "f16s.flac", a, "-s 0.5", md5_of(a, start=22050))
        check_render(vm, "f16s.flac", a, "-s 0 -t 0.001", md5_of(a, start=0, count=44))
        a = corpus["fvar.flac"]
        check_render(vm, "fvar.flac", a, "-s 0.5 -t 0.25", md5_of(a, start=11025, count=5512))
        a = corpus["f24m.flac"]
        check_render(vm, "f24m.flac", a, "-s 0.04 -t 0.02", md5_of(a, start=3840, count=1920))


@test
def audio_fmt_wav_family():
    """WAV (PCM of every size, valid bits in a wider container, float, A-law, mu-law, IMA and Microsoft ADPCM, WAVE_FORMAT_EXTENSIBLE, RF64 and BW64, data sizes of 0
    and 0xFFFFFFFF, odd chunks and tags), AIFF, AIFF-C and Sun .au: each renders to exactly the WAV the host predicts; the kind is found by the first bytes."""
    corpus = wav_corpus()
    esp = audio_esp({"audiofmt/" + k: a.data for k, a in corpus.items()}, "esp-audiofmt-wav")
    with Qemu(cmdline="text", name="t-audiofmt-wav", esp=esp, mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        for name, a in corpus.items():
            check_render(vm, name, a)
        out, rc = sh(vm, "play -i /audiofmt/pcm16s.wav")
        assert "WAV, 44100 Hz, 16-bit, stereo" in out, out
        for line in ("title: Wave Title", "artist: Wave Artist", "album: Wave Album", "track: 4", "date: 2026"):
            assert line in out, f"{line!r} missing from:\n{out}"
        out, rc = sh(vm, "play -i /audiofmt/ima.wav; play -i /audiofmt/msadpcm.wav; play -i /audiofmt/flt32.wav; play -i /audiofmt/alaw.wav")
        for part in ("WAV (IMA ADPCM), 22050 Hz, 16-bit, stereo", "WAV (MS ADPCM), 22050 Hz, 16-bit, mono", "WAV (IEEE float), 44100 Hz, 32-bit, stereo",
                     "WAV (A-law), 8000 Hz, 16-bit, stereo"):
            assert part in out, f"{part!r} missing from:\n{out}"
        out, rc = sh(vm, "play -i /audiofmt/rf64.wav; play -i /audiofmt/pcm16s.aif; play -i /audiofmt/sowt.aifc; play -i /audiofmt/mulaw.au; play -i /audiofmt/ext6.wav")
        for part in ("RF64, 44100 Hz, 16-bit, stereo", "AIFF, 44100 Hz, 16-bit, stereo", "title: Aiff Title", "AIFF-C, 44100 Hz, 16-bit, stereo",
                     "AU (mu-law), 8000 Hz, 16-bit, mono", "WAV, 48000 Hz, 24-bit, 6 channels", "speakers 0x3f"):
            assert part in out, f"{part!r} missing from:\n{out}"
        out, rc = sh(vm, "play -i /audiofmt/noext; play -i /audiofmt/wrong.mp3")
        assert "WAV, 22050 Hz, 16-bit, mono" in out and "AIFF, 22050 Hz, 16-bit, mono" in out, out
        # seeking in the formats that have to find their way into a block or a frame
        for name, start, count in (("ima.wav", 800, 700), ("msadpcm.wav", 600, 900), ("pcm24s.wav", 1000, 500), ("pcm16s.aif", 1234, 111), ("alaw.au", 2000, 999)):
            a = corpus[name]
            # play truncates seconds * rate to whole frames: a thousandth of a frame of margin keeps the decimal text from landing just under the frame
            check_render(vm, name, a, f"-s {(start + 0.001) / a.rate:.9f} -t {(count + 0.001) / a.rate:.9f}", md5_of(a, start=start, count=count))


@test
def audio_fmt_damage():
    """A bad frame in a FLAC file is reported once, skipped and replaced by silence of the same length, the rest plays; a truncated file plays what is there; `play -V`
    says so; an unsupported WAV codec and a junk file are refused with a sentence."""
    n = 8000
    pcm = [fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)]
    clean = flac_audio("dmg", pcm, 44100, 16, blocksize=1000, stereo="auto")
    stream = fe.encode(pcm, 44100, 16, blocksize=1000, stereo="auto")
    frames = stream.frames
    bad = bytearray(clean.data)
    f = frames[3]
    bad[f.start + f.header_len + 4] ^= 0x20                                      # a bit in the body of frame 3
    hdr_bad = bytearray(clean.data)
    hdr_bad[frames[5].start + 4] ^= 0x01                                         # a bit in the header of frame 5 (its CRC-8 fails)
    cut = clean.data[:frames[6].start + 20]                                     # truncated in the middle of frame 6
    junk = bytes(random.Random(1).getrandbits(8) for _ in range(3000))
    mp3 = bytearray(ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16).data)
    mp3[mp3.index(b"fmt ") + 8:mp3.index(b"fmt ") + 10] = (0x55).to_bytes(2, "little")
    ms = ag.wav_msadpcm([ag.ints(1500, 16, 1)], 22050, 256)
    bad_ms = bytearray(ms.data)
    bad_ms[bad_ms.index(b"data") + 8 + 256 * 2] = 99                             # a predictor index that does not exist, in the third block
    files = {"audiofmt/bad.flac": bytes(bad), "audiofmt/badhdr.flac": bytes(hdr_bad), "audiofmt/cut.flac": cut, "audiofmt/junk.flac": junk, "audiofmt/mp3.wav": bytes(mp3),
             "audiofmt/ok.flac": clean.data, "audiofmt/badms.wav": bytes(bad_ms)}
    esp = audio_esp(files, "esp-audiofmt-dmg")

    def silent(a, k0, k1):
        chans = [list(c) for c in a.chans]
        for c in chans:
            c[k0 * 1000:k1 * 1000] = [0] * ((k1 - k0) * 1000)
        return chans

    with Qemu(cmdline="text", name="t-audiofmt-dmg", esp=esp, mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        rc, text, h = play_wav(vm, "bad.flac")
        assert rc == 1 and "1 damaged spot skipped" in text and "checksum" in text, f"exit {rc}: {text}"
        assert text.count("damaged spot") == 1, f"the damage is logged once: {text}"
        assert h == hashlib.md5(ag.render_wav(clean, chans=silent(clean, 3, 4))).hexdigest(), f"frame 3 should be silence and everything else exact: {text}"
        rc, text, h = play_wav(vm, "badhdr.flac")
        assert rc == 1 and "1 damaged spot skipped" in text, text
        assert h == hashlib.md5(ag.render_wav(clean, chans=silent(clean, 5, 6))).hexdigest(), f"frame 5 should be silence and everything else exact: {text}"
        rc, text, h = play_wav(vm, "cut.flac")
        assert rc == 1 and "ends in the middle of a frame" in text, text
        assert h == hashlib.md5(ag.render_wav(clean, count=6000)).hexdigest(), f"the six whole frames should come out: {text}"
        out, rc = sh(vm, "play -V /audiofmt/bad.flac; echo rc=$?; play -V /audiofmt/cut.flac; play -V /audiofmt/ok.flac")
        assert "bad.flac: FAILED" in out and "cut.flac: FAILED" in out and "ok.flac: OK, MD5 matches" in out and "rc=1" in out, out
        out, rc = sh(vm, "play -i /audiofmt/mp3.wav 2>&1; echo rc=$?")
        assert "WAVE format 0x0055 (MP3) is not supported" in out and "rc=1" in out, out
        out, rc = sh(vm, "play -i /audiofmt/junk.flac 2>&1; echo rc=$?")
        assert "need a sound file" in out and "rc=1" in out, out
        rc, text, h = play_wav(vm, "badms.wav")
        assert rc == 1 and "damaged ADPCM block" in text, text
        out, rc = sh(vm, "jukebox --list /audiofmt/bad.flac /audiofmt/mp3.wav /audiofmt/ok.flac /audiofmt/junk.flac 2>&1")
        assert re.search(r"/audiofmt/mp3\.wav -- WAVE format 0x0055 \(MP3\) is not supported", out), out
        assert "/audiofmt/ok.flac" in out and "junk.flac" in out, out


@test
@starved_retry
def audio_fmt_device():
    """Through the real device path (HDA card, capture backend): a FLAC, a 24-bit FLAC, a 6-channel FLAC, an IMA ADPCM WAV, an AIFF and a mu-law AU file are heard at
    the right pitch and level, left and right on their own sides."""
    rate = 44100
    n = rate * 3 // 2
    left, right = fe.tone(n, rate, 440, 0.5, 16), fe.tone(n, rate, 660, 0.5, 16)
    flac16 = flac_audio("dev16", [left, right], rate, 16, blocksize=4096, subframe={"kind": "fixed", "order": 2})
    flac24 = flac_audio("dev24", [fe.tone(48000, 48000, 880, 0.4, 24)], 48000, 24, blocksize=4096, subframe={"kind": "fixed", "order": 2})
    flac6 = flac_audio("dev6", [fe.tone(24000, 48000, 523, 0.3, 16)] * 6, 48000, 16, blocksize=4096, subframe={"kind": "fixed", "order": 2})
    ima = ag.wav_ima([fe.tone(22050, 22050, 1000, 0.5, 16)], 22050, 256)
    aif = ag.aiff([fe.tone(32000, 32000, 1318, 0.5, 24)], 32000, 24)
    ulaw = ag.au([ag.g711_encode(fe.tone(8000, 8000, 700, 0.5, 16), "ulaw")], 8000, 1)
    files = {"audiofmt/dev16.flac": flac16.data, "audiofmt/dev24.flac": flac24.data, "audiofmt/dev6.flac": flac6.data, "audiofmt/ima.wav": ima.data,
             "audiofmt/t.aif": aif.data, "audiofmt/u.au": ulaw.data}
    esp = audio_esp(files, "esp-audiofmt-dev")
    with Qemu(cmdline="text", name="t-audiofmt-dev", esp=esp, mem="512M", audio="hda") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        sh(vm, "volume 100")
        for name in ("dev16.flac", "dev24.flac", "dev6.flac", "ima.wav", "t.aif", "u.au"):
            out, rc = sh(vm, f"play -q /audiofmt/{name}; echo rc=$?; sleep 1", timeout=300)
            assert "rc=0" in out, f"{name}: {out}"
        rate_out, x = capture(vm, check=True, mono=False)
    segs = at.segments(x, rate_out, merge_ms=400)
    assert len(segs) == 6, f"expected six sounds, found {segs}"
    wanted = [(None, 0.5), (880, 0.4), (523, 0.3), (1000, 0.5), (1318.5, 0.5), (700, 0.5)]
    for (a, b), (freq, peak) in zip(segs, wanted):
        seg = x[a:b]
        if freq is None:                                                                  # the stereo FLAC: 440 Hz on the left, 660 Hz on the right
            lf = at.dominant_freq(at.trimmed(seg[:, 0], rate_out), rate_out)
            rf = at.dominant_freq(at.trimmed(seg[:, 1], rate_out), rate_out)
            assert abs(lf - 440) < 5 and abs(rf - 660) < 7, f"left {lf:.1f} Hz and right {rf:.1f} Hz, expected 440 and 660"
            assert 0.42 < at.peak(seg[:, 0]) < 0.55 and 0.42 < at.peak(seg[:, 1]) < 0.55, (at.peak(seg[:, 0]), at.peak(seg[:, 1]))
            continue
        m = at.mono(seg)
        f = at.dominant_freq(at.trimmed(m, rate_out), rate_out)
        assert abs(f - freq) < freq * 0.012 + 2, f"expected {freq} Hz, heard {f:.1f} Hz"
        p = at.peak(at.trimmed(m, rate_out))
        assert abs(p - peak) < 0.07, f"{freq} Hz: peak {p:.2f}, expected {peak}"
        assert float(abs(m[1:] - m[:-1]).max()) < 0.4, f"{freq} Hz: the output has a jump in it"


@test
def audio_fmt_jukebox_list():
    """The Jukebox playlist takes FLAC, WAV, AIFF and AU files: artist and title from the tags, the duration, a file whose name says nothing is looked into, and a file
    that does not parse stays in the list with the decoder's message."""
    f16 = flac_audio("lst1", [fe.tone(44100, 44100, 440, 0.4, 16)] * 2, 44100, 16, comments=["TITLE=First Light", "ARTIST=Reimu"], blocksize=4096,
                     subframe={"kind": "fixed", "order": 2})
    flac_plain = flac_audio("lst2", [fe.tone(22050, 22050, 440, 0.4, 16)], 22050, 16, blocksize=4096, subframe={"kind": "fixed", "order": 2})
    wav = ag.wav_pcm([ag.ints(22050, 16, 1)], 22050, 16, list_info={"INAM": "Wave Song"})
    aif = ag.aiff([ag.ints(44100 * 2, 16, 2)], 44100, 16, tags={"title": "Aiff Song", "artist": "Marisa"})
    au = ag.au([ag.ints(8000 * 3, 16, 3)], 8000, 3)
    mp3 = bytearray(wav.data)
    mp3[mp3.index(b"fmt ") + 8:mp3.index(b"fmt ") + 10] = (0x55).to_bytes(2, "little")
    cut = f16.data[:len(f16.data) // 2]
    files = {"Music/a-first.flac": f16.data, "Music/b-plain.flac": flac_plain.data, "Music/c.wav": wav.data, "Music/d.aif": aif.data, "Music/e.au": au.data,
             "Music/f-unsupported.wav": bytes(mp3), "Music/g-cut.flac": cut, "Music/h.txt": b"not music", "elsewhere/nameless": wav.data,
             "Music/Album One/01-intro.flac": f16.data, "Music/Album One/02-second.flac": flac_plain.data, "Music/Album Two/Disc 2/deep.wav": wav.data}
    esp = audio_esp({"audiofmt/" + k: v for k, v in files.items()}, "esp-audiofmt-list")
    with Qemu(cmdline="text", name="t-audiofmt-list", esp=esp, mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        # HOME=/audiofmt: the Jukebox scans /audiofmt/Music by name (the .txt is not picked up). The order is the files named on the command line (this one is looked
        # into whatever its name), the songs that ship with the system, then ~/Music.
        out, rc = sh(vm, "HOME=/audiofmt jukebox --list /audiofmt/elsewhere/nameless 2>&1")
        assert rc == 0, out
        want = [
            r"^\s*1\. Wave Song \(0:01\) /audiofmt/elsewhere/nameless$",
            r"^\s*\d+\. Reimu - First Light \(0:01\) /audiofmt/Music/a-first\.flac$",
            r"^\s*\d+\. b-plain \(0:01\) /audiofmt/Music/b-plain\.flac$",
            r"^\s*\d+\. Wave Song \(0:01\) /audiofmt/Music/c\.wav$",
            r"^\s*\d+\. Marisa - Aiff Song \(0:02\) /audiofmt/Music/d\.aif$",
            r"^\s*\d+\. e \(0:03\) /audiofmt/Music/e\.au$",
            r"^\s*\d+\. f-unsupported \(0:00\) /audiofmt/Music/f-unsupported\.wav -- WAVE format 0x0055 \(MP3\) is not supported$",
            r"^\s*\d+\. Reimu - First Light \(0:01\) /audiofmt/Music/g-cut\.flac$",     # a stream cut in half still opens, with its tags and the length its header gives: its frames are what is missing
            r"^\s*\d+\. Reimu - First Light \(0:01\) /audiofmt/Music/Album One/01-intro\.flac$",              # an album in its own folder, and a folder inside a folder
            r"^\s*\d+\. 02-second \(0:01\) /audiofmt/Music/Album One/02-second\.flac$",
            r"^\s*\d+\. Wave Song \(0:01\) /audiofmt/Music/Album Two/Disc 2/deep\.wav$",
        ]
        for pat in want:
            assert re.search(pat, out, re.M), f"{pat}\n--\n{out}"
        assert "h.txt" not in out, out
        out, rc = sh(vm, "jukebox --list /audiofmt/Music/h.txt /audiofmt/Music/c.wav 2>&1 | head -3")
        assert "h.txt: not music the Jukebox plays" in out and "c.wav" in out, out
        out, rc = sh(vm, "jukebox --list '/audiofmt/Music/Album Two' 2>&1 | head -2")                     # a folder named on the command line is searched the same way
        assert re.search(r"^\s*1\. Wave Song \(0:01\) /audiofmt/Music/Album Two/Disc 2/deep\.wav$", out, re.M), out


@test
def audio_fmt_flac_speed():
    """How fast the guest decodes FLAC: 30 seconds of 16-bit stereo 44.1 kHz audio decoded and checked (`play -V`), and rendered to a WAV (`play -w`); the speeds in
    times real time are printed. They have to beat real time with room to spare (the Jukebox decodes while it plays)."""
    secs = 30
    n = 44100 * secs
    a = flac_audio("speed", [fe.music(n, 44100, 16, 21), fe.music(n, 44100, 16, 22)], 44100, 16, blocksize=4096, stereo="mid_side", subframe={"kind": "lpc", "order": 8, "precision": 12},
                   seek_every=44100 * 10)
    esp = audio_esp({"audiofmt/speed.flac": a.data}, "esp-audiofmt-speed")
    with Qemu(cmdline="text", name="t-audiofmt-speed", esp=esp, mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        out, rc = sh(vm, "cat /proc/uptime")
        assert re.match(r"\s*(\d+\.\d+)\s", out), out

        def uptime():
            o, _ = sh(vm, "cat /proc/uptime")
            return float(re.match(r"\s*(\d+\.\d+)", o.strip()).group(1))

        results = {}
        for label, cmd in (("verify", "play -V /audiofmt/speed.flac"), ("render", "play -q -w /tmp/s.wav /audiofmt/speed.flac")):
            t0 = uptime()
            out, rc = sh(vm, cmd, timeout=900)
            t1 = uptime()
            assert rc == 0, f"{cmd}: {out}"
            results[label] = secs / max(t1 - t0, 0.001)
        out, rc = sh(vm, "ls -l /audiofmt/speed.flac")
        size = int(re.search(r"\s(\d{5,})\s", out).group(1))
    print(f"\n     decode speed of 16-bit stereo 44.1 kHz FLAC in the guest ({size / 1024 / secs:.0f} KiB/s of FLAC): `play -V` {results['verify']:.0f}x real time "
          f"(decoding and the MD5), `play -w` {results['render']:.0f}x (decoding, packing and writing a WAV file)")
    assert results["verify"] > 3 and results["render"] > 3, f"decoding is slower than 3x real time: {results}"


# --------------------------------------------------------------------------------------------------------------- the Jukebox

def jukebox_tracks():
    n = 44100 * 3
    a = flac_audio("jb_a", [fe.tone(n, 44100, 440, 0.5, 16)] * 2, 44100, 16, comments=["TITLE=Tone A", "ARTIST=Lunasa"], blocksize=4096, subframe={"kind": "fixed", "order": 2})
    b = flac_audio("jb_b", [fe.tone(n, 44100, 660, 0.5, 16)] * 2, 44100, 16, comments=["TITLE=Tone B", "ARTIST=Merlin"], blocksize=4096, subframe={"kind": "fixed", "order": 2})
    return a, b


@test
def audio_fmt_jukebox_gapless():
    """Two FLAC files of the same format in the Jukebox: the second follows the first without the sound card ever running dry (one stretch of sound, no silence between
    the tones), the window and the trace say which one is playing, and the tags are shown."""
    from guitools import Gui
    a, b = jukebox_tracks()
    esp = audio_esp({"audiofmt/ga.flac": a.data, "audiofmt/gb.flac": b.data}, "esp-audiofmt-gapless")

    def attempt():
        with Qemu(cmdline="text", name="t-audiofmt-gapless", esp=esp, mem="768M", audio="hda") as vm:
            vm.expect(test_shell.PROMPT, timeout=120)
            vm.send("\n")
            g = Gui(vm)
            g.start_desktop()
            vm.send("volume 100; jukebox --trace -p /audiofmt/ga.flac /audiofmt/gb.flac &\n")
            vm.expect(r"jukebox: ready with 7 tracks", timeout=180)
            vm.expect(r"jukebox: playing Lunasa - Tone A", timeout=60)
            vm.expect(r"jukebox: finished Lunasa - Tone A", timeout=120)
            vm.expect(r"jukebox: playing Merlin - Tone B", timeout=30)
            time.sleep(scaled(2.5))                                              # (no screenshot here: taking one stalls the emulated card for a moment and the recording gets a click)
            vm.key("q", ctrl=True)
            time.sleep(1)
            return capture(vm, check=True)

    for tries in range(3):
        case_audio.STARVED[0] = False
        rate, x = attempt()
        segs = at.segments(x, rate, threshold=0.004, merge_ms=25, min_ms=300)
        if len(segs) == 1:
            break
        starved = bool(case_audio.STARVED[0])                                  # the guest's mixer ran dry for a moment: the host was busy, try again
        assert starved and tries < 2, f"the two tones are not one stretch of sound (a gap between them): {segs}"
        case_audio.STARVED[0] = False
    (s0, s1), = segs
    dur = (s1 - s0) / rate
    assert dur > 4.5, f"tone A (3 s) and a part of tone B should be one stretch of sound, found {dur:.1f} s"
    first = x[s0 + int(0.3 * rate):s0 + int(2.5 * rate)]
    last = x[s0 + int(3.4 * rate):s1 - int(0.2 * rate)]
    fa, fb = at.dominant_freq(first, rate), at.dominant_freq(last, rate)
    assert abs(fa - 440) < 5 and abs(fb - 660) < 7, f"the first tone should be 440 Hz and the second 660 Hz, heard {fa:.1f} and {fb:.1f}"
    # The last moment of the stretch is the test quitting the Jukebox in the middle of a tone: the signal falls from the tone's level to silence there, which is not a click
    # of the playback (a capture of this test showed exactly that jump, 0.497, as the only one; across the 3 s boundary of the two tones the biggest step was 0.043).
    jumps = np.abs(np.diff(x[s0:s1 - int(0.1 * rate)]))
    at_ = int(jumps.argmax())
    assert float(jumps.max()) < 0.4, (f"a click in the music: a jump of {float(jumps.max()):.3f} at {at_ / rate:.3f} s of a {(s1 - s0) / rate:.3f} s stretch "
                                      f"(samples around it {np.round(x[s0 + max(at_ - 3, 0):s0 + at_ + 5], 3).tolist()})")
