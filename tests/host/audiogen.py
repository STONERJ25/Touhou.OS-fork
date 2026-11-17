"""audiogen.py: makes the test files of the WAV family (WAVE in all its kinds, RF64 and BW64, AIFF and AIFF-C, Sun .au) on the host, in plain Python, and says
what each one must decode to. Used by tests/host/test_audio_formats.py (the C readers in WSL, with Python's wave, aifc, sunau and audioop modules as
independent references) and by tests/case_audio_formats.py (the same files in the QEMU guest).

Every maker returns an `Audio`: `data` is the file, `chans` the samples the reader must give back (one list of ints per channel, right-justified at `bits`:
what is stored, shifted down to the significant bits; 16 for G.711 and ADPCM, 32 for floating point), and the facts a reader reports (rate, mask, tags).
`delivered(audio)` is what the delivered (device) format holds: the samples packed into 8, 16, 24 or 32 bits, left-justified, 8 bits unsigned.

The two ADPCM codecs are written here from their block formats (an encoder to make files, a decoder that is the model of what the C code must give back).
Python's audioop checks the IMA decoder independently (tests/host/test_audio_formats.py); nothing independent is installed for Microsoft ADPCM, which is
checked against this model only (an encoder and a decoder written from one reading of the specification), and G.711 is checked against audioop.
"""
import math
import random
import struct
from dataclasses import dataclass, field

import flacenc

tone, noise, music = flacenc.tone, flacenc.noise, flacenc.music

SPEAKERS = {"FL": 0x1, "FR": 0x2, "FC": 0x4, "LFE": 0x8, "BL": 0x10, "BR": 0x20, "FLC": 0x40, "FRC": 0x80, "BC": 0x100, "SL": 0x200, "SR": 0x400}


@dataclass
class Audio:
    data: bytes
    chans: list                                      # decoded samples, one list per channel
    rate: int
    bits: int                                        # significant bits of what the reader reports
    fmt: str = ""                                    # "WAV", "RF64", "BW64", "AIFF", "AIFF-C", "AU"
    codec: str = "PCM"
    mask: int = 0
    tags: dict = field(default_factory=dict)         # title, artist, album, track, date
    frames: int = 0                                  # what the reader reports as the length (len(chans[0]) unless the test says otherwise)

    def __post_init__(self):
        if not self.frames:
            self.frames = len(self.chans[0]) if self.chans else 0

    @property
    def channels(self):
        return len(self.chans)


def container_of(bits):
    return 8 if bits <= 8 else 16 if bits <= 16 else 24 if bits <= 24 else 32


def delivered(a, chans=None, bits=None):
    """The bytes afile_read delivers for these samples: each sample shifted left into its container, little endian, 8-bit unsigned."""
    chans = a.chans if chans is None else chans
    bits = a.bits if bits is None else bits
    c = container_of(bits)
    n = len(chans[0]) if chans else 0
    out = bytearray()
    for i in range(n):
        for ch in chans:
            v = ch[i] << (c - bits)
            if c == 8:
                out.append((v + 128) & 0xFF)
            else:
                out += (v & ((1 << c) - 1)).to_bytes(c // 8, "little")
    return bytes(out)


def render_wav(a, start=0, count=None, chans=None):
    """The file afile_render_wav writes (and so `play -w`) for this Audio: all channels, the file's own rate, each sample shifted left into its container; plain PCM
    for mono and stereo of 8 or 16 bits, WAVE_FORMAT_EXTENSIBLE (with the speaker mask and the valid bits) otherwise. `start` and `count` cut a part (`play -s -t`)."""
    chans = a.chans if chans is None else chans
    end = None if count is None else start + count
    part = [list(c[start:end]) for c in chans]
    c = container_of(a.bits)
    body = delivered(a, part)
    nch = len(part)
    ext = nch > 2 or c > 16 or a.bits != c
    block = nch * c // 8
    h = b"RIFF" + struct.pack("<I", (60 if ext else 36) + len(body) + (len(body) & 1)) + b"WAVEfmt " + struct.pack("<I", 40 if ext else 16)
    if ext:
        h += struct.pack("<HHIIHHHHIH", 0xFFFE, nch, a.rate, a.rate * block, block, c, 22, a.bits, a.mask, 1) + bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    else:
        h += struct.pack("<HHIIHH", 1, nch, a.rate, a.rate * block, block, c)
    h += b"data" + struct.pack("<I", len(body))
    return h + body + (b"\0" if len(body) & 1 else b"")


def downmix_gains(channels, mask):
    """The Q16 gains of each channel into left and right: the model of afile.c's build_downmix (same formulas, same order of operations)."""
    c, h = 0.7071067811865476, 0.5
    table = {0: (1, 0), 1: (0, 1), 2: (c, c), 3: (0, 0), 4: (c, 0), 5: (0, c), 6: (c, 0), 7: (0, c), 8: (h, h), 9: (c, 0), 10: (0, c), 11: (h, h), 12: (c, 0),
             13: (h, h), 14: (0, c), 15: (h, 0), 16: (0.35, 0.35), 17: (0, h)}
    layouts = {1: 0x4, 2: 0x3, 3: 0x7, 4: 0x33, 5: 0x37, 6: 0x3F, 7: 0x70F, 8: 0x63F}
    if not mask or bin(mask).count("1") != channels:
        mask = layouts.get(channels, 0x3)
    gl, gr, sl, sr = [], [], 0.0, 0.0
    for bit in range(32):
        if len(gl) >= channels:
            break
        if mask & (1 << bit):
            l, r = table.get(bit, (0, 0))
            gl.append(float(l))
            gr.append(float(r))
            sl += l
            sr += r
    while len(gl) < channels:
        gl.append(0.0)
        gr.append(0.0)
    norm = max(sl, sr, 1.0)
    return [int(g / norm * 65536.0 + 0.5) for g in gl], [int(g / norm * 65536.0 + 0.5) for g in gr]


def downmixed(a):
    """The stereo samples afile_read delivers for a file of more than two channels."""
    gl, gr = downmix_gains(a.channels, a.mask)
    left, right = [], []
    for i in range(len(a.chans[0])):
        l = sum(ch[i] * g for ch, g in zip(a.chans, gl))
        r = sum(ch[i] * g for ch, g in zip(a.chans, gr))
        left.append((l + 32768) >> 16)
        right.append((r + 32768) >> 16)
    return [left, right]


# ---------------------------------------------------------------------------------------------------------------- G.711

def ulaw_decode(b):
    b = ~b & 0xFF
    t = (((b & 0x0F) << 3) + 0x84) << ((b & 0x70) >> 4)
    return 0x84 - t if b & 0x80 else t - 0x84


def alaw_decode(b):
    b ^= 0x55
    t = (b & 0x0F) << 4
    seg = (b & 0x70) >> 4
    if seg == 0:
        t += 8
    else:
        t = (t + 0x108) << (seg - 1)
    return t if b & 0x80 else -t


def g711_encode(samples, kind="ulaw"):
    """The G.711 byte whose decoded value is nearest to each 16-bit sample (brute force over the 256 codes: slow but obviously right)."""
    import bisect
    dec = ulaw_decode if kind == "ulaw" else alaw_decode
    table = sorted((dec(b), b) for b in range(256))
    values = [v for v, _ in table]
    out = []
    for s in samples:
        i = bisect.bisect_left(values, s)
        cands = [j for j in (i - 1, i) if 0 <= j < len(table)]
        out.append(table[min(cands, key=lambda j: abs(values[j] - s))][1])
    return out


def float_to_s32(x):
    """The reader's conversion of a floating point sample to a 32-bit integer: rounded, clipped, NaN is zero."""
    if x != x:
        return 0
    t = x * 2147483648.0 + 0.5
    if t >= 2147483647.0:
        return 2147483647
    if t <= -2147483648.0:
        return -2147483648
    return math.floor(t)


# ------------------------------------------------------------------------------------------------------------------ ADPCM

IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
            253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
            3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
            32767]
IMA_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_step(nib, pred, index):
    step = IMA_STEP[index]
    diff = step >> 3
    if nib & 4:
        diff += step
    if nib & 2:
        diff += step >> 1
    if nib & 1:
        diff += step >> 2
    pred += -diff if nib & 8 else diff
    pred = max(-32768, min(32767, pred))
    index = max(0, min(88, index + IMA_ADJUST[nib & 7]))
    return pred, index


def ima_encode_nibble(sample, pred, index):
    step = IMA_STEP[index]
    diff = sample - pred
    nib = 0
    if diff < 0:
        nib = 8
        diff = -diff
    if diff >= step:
        nib |= 4
        diff -= step
    if diff >= step >> 1:
        nib |= 2
        diff -= step >> 1
    if diff >= step >> 2:
        nib |= 1
    return nib


def wav_ima(chans, rate, block_align=None, *, fact=True, partial_last=False, extra_chunks=b"", list_info=None):
    """WAVE_FORMAT_IMA_ADPCM (0x11): blocks of 4 header bytes per channel and groups of eight 4-bit samples per channel. `chans` are 16-bit samples; the file's
    expected decode is what a decoder gives back (the model below), which is not the input (the codec is lossy)."""
    nch = len(chans)
    block_align = block_align or 256 * nch
    spb = (block_align - 4 * nch) // (4 * nch) * 8 + 1
    n = len(chans[0])
    blocks, decoded = bytearray(), [[] for _ in range(nch)]
    pos = 0
    while pos < n:
        take = min(spb, n - pos)
        padded = [list(c[pos:pos + take]) + [c[pos + take - 1]] * (spb - take) for c in chans]
        hdr, nibs, state = bytearray(), [], []
        for c in range(nch):
            pred, index = padded[c][0], 0
            hdr += struct.pack("<hBB", pred, index, 0)
            state.append((pred, index))
            decoded[c].append(pred)
            nibs.append([])
        for c in range(nch):
            pred, index = state[c]
            for s in padded[c][1:]:
                nib = ima_encode_nibble(s, pred, index)
                pred, index = ima_step(nib, pred, index)
                nibs[c].append(nib)
                decoded[c].append(pred)
        body = bytearray()
        for g in range((spb - 1) // 8):
            for c in range(nch):
                for j in range(0, 8, 2):
                    body.append(nibs[c][g * 8 + j] | (nibs[c][g * 8 + j + 1] << 4))                # the earlier sample in the low nibble
        blocks += (hdr + body).ljust(block_align, b"\0")
        pos += take
    exact = n
    chans_out = [d[:exact] if fact else d for d in decoded]
    fmt = struct.pack("<HHIIHH", 0x11, nch, rate, rate * block_align // spb, block_align, 4) + struct.pack("<HH", 2, spb)
    chunks = _chunk(b"fmt ", fmt)
    if fact:
        chunks += _chunk(b"fact", struct.pack("<I", exact))
    chunks += extra_chunks + _chunk(b"data", bytes(blocks))
    if list_info:
        chunks += _list_info(list_info)
    return Audio(_riff(chunks), chans_out, rate, 16, "WAV", "IMA ADPCM", 0, _tags(list_info))


MS_COEF = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
MS_ADAPT = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]


def _ms_predict(s1, s2, c):
    num = s1 * c[0] + s2 * c[1]
    return num >> 8                                      # an arithmetic shift (floor), as Windows' codec does it; C's division would round toward zero


def wav_msadpcm(chans, rate, block_align=None, *, coefs=MS_COEF, fact=True):
    """WAVE_FORMAT_ADPCM (2), Microsoft's: a 7-byte header per channel (predictor, delta, two samples) and 4-bit errors, the high nibble first."""
    nch = len(chans)
    block_align = block_align or 256 * nch
    spb = (block_align - 7 * nch) * 2 // nch + 2
    n = len(chans[0])
    blocks, decoded = bytearray(), [[] for _ in range(nch)]
    pos = 0
    while pos < n:
        take = min(spb, n - pos)
        padded = [list(c[pos:pos + take]) + [c[pos + take - 1]] * (spb - take) for c in chans]
        pidx, delta, s1, s2 = [], [], [], []
        for c in range(nch):
            best = min(range(len(coefs)), key=lambda k: sum(abs(padded[c][i] - _ms_predict(padded[c][i - 1], padded[c][i - 2], coefs[k])) for i in range(2, min(spb, 40))))
            pidx.append(best)
            delta.append(max(16, abs(padded[c][1] - padded[c][0]) // 4))
            s2.append(padded[c][0])
            s1.append(padded[c][1])
        hdr = bytes(pidx) + b"".join(struct.pack("<h", d) for d in delta) + b"".join(struct.pack("<h", v) for v in s1) + b"".join(struct.pack("<h", v) for v in s2)
        for c in range(nch):
            decoded[c] += [s2[c], s1[c]]
        nibs = []
        for i in range(2, spb):
            for c in range(nch):
                pred = _ms_predict(s1[c], s2[c], coefs[pidx[c]])
                err = padded[c][i] - pred
                q = int(round(err / delta[c])) if delta[c] else 0
                q = max(-8, min(7, q))
                nib = q & 15
                v = max(-32768, min(32767, pred + q * delta[c]))
                decoded[c].append(v)
                s2[c], s1[c] = s1[c], v
                delta[c] = max(16, (MS_ADAPT[nib] * delta[c]) >> 8)
                nibs.append(nib)
        if len(nibs) % 2:
            nibs.append(0)
        body = bytes((nibs[i] << 4) | nibs[i + 1] for i in range(0, len(nibs), 2))
        blocks += (hdr + body).ljust(block_align, b"\0")
        pos += take
    chans_out = [d[:n] if fact else d for d in decoded]
    cf = b"".join(struct.pack("<hh", a, b) for a, b in coefs)
    fmt = struct.pack("<HHIIHH", 2, nch, rate, rate * block_align // spb, block_align, 4) + struct.pack("<HHH", 32, spb, len(coefs)) + cf
    chunks = _chunk(b"fmt ", fmt)
    if fact:
        chunks += _chunk(b"fact", struct.pack("<I", n))
    chunks += _chunk(b"data", bytes(blocks))
    return Audio(_riff(chunks), chans_out, rate, 16, "WAV", "MS ADPCM")


# -------------------------------------------------------------------------------------------------------------------- WAVE

def _chunk(cid, body):
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def _riff(chunks, kind=b"RIFF", size=None):
    return kind + struct.pack("<I", len(chunks) + 4 if size is None else size) + b"WAVE" + chunks


def _list_info(tags):
    """tags: {"INAM": "title", ...} as a LIST/INFO chunk; strings are NUL terminated (and padded to an even length by _chunk)."""
    body = b"INFO"
    for k, v in tags.items():
        raw = v if isinstance(v, bytes) else v.encode()
        body += _chunk(k.encode(), raw + b"\0")
    return _chunk(b"LIST", body)


def text_model(raw, cap=128):
    """What afile.c's af_text makes of a tag: up to the first NUL, UTF-8 kept (anything else is Latin-1 and converted), control characters blanked, trailing blanks
    dropped, cut to cap - 1 bytes at a character boundary."""
    raw = raw.encode() if isinstance(raw, str) else raw
    raw = raw.split(b"\0")[0]
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("latin-1")
    text = "".join(" " if ord(c) < 0x20 or ord(c) == 0x7F else c for c in text)
    out, size = "", 0
    for c in text:
        w = len(c.encode())
        if size + w > cap - 1:
            break
        out += c
        size += w
    return out.rstrip(" ")


def _tags(info):
    names = {"INAM": ("title", 128), "IART": ("artist", 128), "IPRD": ("album", 128), "ITRK": ("track", 16), "IPRT": ("track", 16), "ICRD": ("date", 32)}
    return {names[k][0]: text_model(v, names[k][1]) for k, v in (info or {}).items() if k in names}


def pcm_bytes(chans, container, valid, *, big=False, unsigned8=False):
    """Interleaved samples (right-justified at `valid` bits) stored left-justified in `container` bytes."""
    out = bytearray()
    shift = container * 8 - valid
    for i in range(len(chans[0])):
        for ch in chans:
            v = ch[i] << shift
            if container == 1 and unsigned8:
                out.append((v + 128) & 0xFF)
            else:
                out += (v & ((1 << (8 * container)) - 1)).to_bytes(container, "big" if big else "little")
    return bytes(out)


def wav_pcm(chans, rate, bits, *, container=None, extensible=False, mask=0, fmt_bytes=16, list_info=None, data_size="exact", before_data=b"", after_data=b"",
            riff_size=None, kind=b"RIFF", ds64=False):
    """WAVE_FORMAT_PCM. `bits` are the significant bits and `container` the bits in a sample's bytes (default: the next whole byte). `extensible` writes
    WAVEFORMATEXTENSIBLE with the speaker mask. `fmt_bytes` 16 or 18 (with a zero cbSize) for the plain header. data_size: 'exact', 'zero', 'ffff'
    (0xFFFFFFFF), 'larger' (promises more than the file holds). RF64 / BW64 need kind=b"RF64"/b"BW64" and ds64=True."""
    nch = len(chans)
    container = container or container_of(bits)
    cb = container // 8
    body = pcm_bytes(chans, cb, bits, unsigned8=(cb == 1))
    block = nch * cb
    if extensible:
        fmt = struct.pack("<HHIIHH", 0xFFFE, nch, rate, rate * block, block, container) + struct.pack("<HHIH", 22, bits, mask, 1) + bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    else:
        fmt = struct.pack("<HHIIHH", 1, nch, rate, rate * block, block, bits if container * 1 >= bits else container)
        if fmt_bytes == 18:
            fmt += struct.pack("<H", 0)
    return _wave(fmt, body, rate, data_size, before_data, after_data, list_info, riff_size, kind, ds64, Audio(b"", [list(c) for c in chans], rate, bits, "WAV", "PCM", mask if extensible else 0))


def wav_float(samples, rate, bits=32, *, extensible=False, mask=0, **kw):
    """WAVE_FORMAT_IEEE_FLOAT, 32 or 64 bits: `samples` are floats; the expected decode is float_to_s32 of each."""
    nch = len(samples)
    fmt_t = "<f" if bits == 32 else "<d"
    body = bytearray()
    for i in range(len(samples[0])):
        for ch in samples:
            body += struct.pack(fmt_t, ch[i])
    block = nch * bits // 8
    if extensible:
        fmt = struct.pack("<HHIIHH", 0xFFFE, nch, rate, rate * block, block, bits) + struct.pack("<HHIH", 22, bits, mask, 3) + bytes([0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    else:
        fmt = struct.pack("<HHIIHH", 3, nch, rate, rate * block, block, bits) + struct.pack("<H", 0)
    exp = [[float_to_s32(struct.unpack(fmt_t, struct.pack(fmt_t, v))[0]) for v in ch] for ch in samples]
    return _wave(bytes(fmt), bytes(body), rate, kw.get("data_size", "exact"), kw.get("before_data", b""), kw.get("after_data", b""), kw.get("list_info"),
                 kw.get("riff_size"), kw.get("kind", b"RIFF"), kw.get("ds64", False), Audio(b"", exp, rate, 32, "WAV", "IEEE float", mask if extensible else 0))


def wav_g711(codes, rate, kind="alaw", **kw):
    """WAVE_FORMAT_ALAW (6) / WAVE_FORMAT_MULAW (7); `codes` are the bytes (one list per channel), any value is a valid code."""
    nch = len(codes)
    tag = 6 if kind == "alaw" else 7
    dec = alaw_decode if kind == "alaw" else ulaw_decode
    body = bytearray()
    for i in range(len(codes[0])):
        for ch in codes:
            body.append(ch[i])
    fmt = struct.pack("<HHIIHH", tag, nch, rate, rate * nch, nch, 8) + struct.pack("<H", 0)
    exp = [[dec(b) for b in ch] for ch in codes]
    return _wave(fmt, bytes(body), rate, kw.get("data_size", "exact"), kw.get("before_data", b""), kw.get("after_data", b""), kw.get("list_info"), None, b"RIFF", False,
                 Audio(b"", exp, rate, 16, "WAV", "A-law" if kind == "alaw" else "mu-law"))


def _wave(fmt, body, rate, data_size, before_data, after_data, list_info, riff_size, kind, ds64, audio):
    chunks = b""
    if ds64:
        chunks += _chunk(b"ds64", struct.pack("<QQQI", 0, len(body), 0, 0))
    chunks += _chunk(b"fmt ", fmt) + before_data
    size = {"exact": len(body), "zero": 0, "ffff": 0xFFFFFFFF, "larger": len(body) + 4096}[data_size]
    if ds64:
        size = 0xFFFFFFFF
    chunks += b"data" + struct.pack("<I", size) + body + (b"\0" if len(body) & 1 and data_size == "exact" else b"")
    chunks += after_data
    if list_info:
        chunks += _list_info(list_info)
    if ds64:
        # fix the ds64 sizes now that the whole file is known
        total = len(chunks) + 4
        chunks = _chunk(b"ds64", struct.pack("<QQQI", total, len(body), len(body) // max(1, audio.channels * (audio.bits + 7 // 8)), 0)) + chunks[8 + 28:]
    data = _riff(chunks, kind, 0xFFFFFFFF if ds64 else riff_size)
    audio.data = data
    audio.tags = _tags(list_info)
    if kind in (b"RF64", b"BW64"):
        audio.fmt = kind.decode()
    return audio


# -------------------------------------------------------------------------------------------------------------------- AIFF

def ext80(rate):
    """An integer as the 10-byte IEEE 754 extended number of AIFF."""
    e = rate.bit_length() - 1
    return struct.pack(">HQ", 16383 + e, rate << (63 - e))


def _iff(cid, body):
    return cid + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def aiff(chans, rate, bits, *, comp=None, tags=None, odd_pad=False, ssnd_offset=0, extra=b"", declared_frames=None):
    """AIFF (comp None) or AIFF-C (comp 'NONE', 'sowt', 'fl32', 'FL32', 'fl64', 'FL64', 'ulaw', 'ULAW', 'alaw', 'ALAW', 'raw ', 'twos', 'in24', 'in32'). Samples
    of PCM are `bits` significant bits (1 to 32) right-justified in `chans` and stored big endian, left-justified in ceil(bits / 8) bytes."""
    nch = len(chans)
    n = len(chans[0])
    frames = n if declared_frames is None else declared_frames
    aifc = comp is not None
    kind = comp or "NONE"
    if kind in ("NONE", "twos", "in24", "in32", "sowt"):
        cb = (bits + 7) // 8
        body = pcm_bytes(chans, cb, bits, big=(kind != "sowt"))
        exp, ebits, codec, size_field = [list(c) for c in chans], bits, "PCM", bits
    elif kind == "raw ":
        body = bytes(((ch[i] + 128) & 0xFF) for i in range(n) for ch in chans)
        exp, ebits, codec, size_field = [list(c) for c in chans], 8, "PCM", 8
    elif kind in ("fl32", "FL32", "fl64", "FL64"):
        fmt_t = ">f" if kind.endswith("32") else ">d"
        body = b"".join(struct.pack(fmt_t, ch[i]) for i in range(n) for ch in chans)
        exp = [[float_to_s32(struct.unpack(fmt_t, struct.pack(fmt_t, v))[0]) for v in ch] for ch in chans]
        ebits, codec, size_field = 32, "IEEE float", 32 if kind.endswith("32") else 64
    else:                                                                      # ulaw / alaw: chans are the code bytes
        body = bytes(ch[i] for i in range(n) for ch in chans)
        dec = ulaw_decode if kind.lower() == "ulaw" else alaw_decode
        exp = [[dec(b) for b in ch] for ch in chans]
        ebits, codec, size_field = 16, "mu-law" if kind.lower() == "ulaw" else "A-law", 16
    if declared_frames is not None and declared_frames < n:
        exp = [c[:declared_frames] for c in exp]
    comm = struct.pack(">hIh", nch, frames, size_field) + ext80(rate)
    if aifc:
        name = {"NONE": "not compressed", "sowt": "", "fl32": "32-bit floating point", "ulaw": "CCITT G.711 u-law"}.get(kind, "")
        comm += kind.encode().ljust(4)[:4] + bytes([len(name)]) + name.encode() + (b"\0" if (len(name) + 1) % 2 else b"")
    chunks = _iff(b"COMM", comm)
    for k, v in (tags or {}).items():
        chunks += _iff({"title": b"NAME", "artist": b"AUTH"}[k], v.encode() if isinstance(v, str) else v)
    chunks += extra
    chunks += _iff(b"SSND", struct.pack(">II", ssnd_offset, 0) + bytes(ssnd_offset) + body)
    form = b"FORM" + struct.pack(">I", len(chunks) + 4) + (b"AIFC" if aifc else b"AIFF") + chunks
    return Audio(form, exp, rate, ebits, "AIFF-C" if aifc else "AIFF", codec, 0, dict(tags or {}), frames=n if declared_frames is None else min(n, declared_frames))


# ----------------------------------------------------------------------------------------------------------------------- AU

def au(chans, rate, enc, *, annotation=b"", data_size="exact"):
    """Sun .au: enc 1 (mu-law, chans are code bytes), 2 (8-bit), 3 (16), 4 (24), 5 (32) linear PCM, 6 (float 32), 7 (float 64), 27 (A-law)."""
    nch = len(chans)
    n = len(chans[0])
    if enc in (2, 3, 4, 5):
        cb = {2: 1, 3: 2, 4: 3, 5: 4}[enc]
        bits = cb * 8
        body = pcm_bytes(chans, cb, bits, big=True)
        exp, ebits, codec = [list(c) for c in chans], bits, "PCM"
    elif enc in (6, 7):
        fmt_t = ">f" if enc == 6 else ">d"
        body = b"".join(struct.pack(fmt_t, ch[i]) for i in range(n) for ch in chans)
        exp = [[float_to_s32(struct.unpack(fmt_t, struct.pack(fmt_t, v))[0]) for v in ch] for ch in chans]
        ebits, codec = 32, "IEEE float"
    else:
        body = bytes(ch[i] for i in range(n) for ch in chans)
        dec = ulaw_decode if enc == 1 else alaw_decode
        exp = [[dec(b) for b in ch] for ch in chans]
        ebits, codec = 16, "mu-law" if enc == 1 else "A-law"
    off = 24 + len(annotation)
    size = {"exact": len(body), "unknown": 0xFFFFFFFF, "larger": len(body) + 1000}[data_size]
    head = b".snd" + struct.pack(">IIIII", off, size, enc, rate, nch) + annotation
    return Audio(head + body, exp, rate, ebits, "AU", codec)


# ------------------------------------------------------------------------------------------------------------ test signals

def ints(n, bits, seed=1, amp=0.8):
    """A mix of tones and noise in `bits` bits: something with a spectrum, not too clean."""
    return music(n, 44100, bits, seed)


def codes(n, seed=1):
    rnd = random.Random(seed)
    return [rnd.randrange(256) for _ in range(n)]
