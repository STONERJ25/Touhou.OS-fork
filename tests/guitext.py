"""Reading text off a screenshot: the desktop draws every string with the 8x16 bitmap font in shared/font8x16.c, so a test
can render the string it expects with the same font and look for it, pixel for pixel, in the colour the program uses.

    from guitext import find_text
    hit = find_text(vm.screen_ppm(), "Now playing: Bamboo Lantern Walk", (0x8E, 0x0B, 0x21), region=(280, 280, 380, 40))
    assert hit, "the title is not on the screen"
"""
import re
from pathlib import Path

import numpy as np

_FONT = None


def font():
    """{code point: 16 row bytes}, parsed from the generated C table."""
    global _FONT
    if _FONT is None:
        src = (Path(__file__).resolve().parent.parent / "shared" / "font8x16.c").read_text(encoding="utf-8", errors="replace")
        _FONT = {}
        for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4}),\s*\{([^}]*)\}\s*\}", src):
            rows = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
            if len(rows) == 16:
                _FONT[int(m.group(1), 16)] = rows
    return _FONT


def text_mask(text):
    """Boolean array (16, 8 * len(text)): True where the font draws ink."""
    f = font()
    cols = []
    for ch in text:
        rows = f.get(ord(ch))
        if rows is None:
            raise ValueError(f"no glyph for {ch!r}")
        g = np.zeros((16, 8), dtype=bool)
        for y, byte in enumerate(rows):
            for x in range(8):
                g[y, x] = bool(byte & (0x80 >> x))
        cols.append(g)
    return np.concatenate(cols, axis=1)


def image(shot):
    w, h, rgb = shot
    return np.frombuffer(rgb, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3).astype(np.int16)


def find_text(shot, text, color, region=None, tol=36, ink=0.97, clean=0.06):
    """Looks for `text` drawn in `color` ((r, g, b)) inside region = (x, y, w, h) of the screenshot. Returns (x, y) of its top
    left corner, or None. At least `ink` of the glyph pixels must have the colour and at most `clean` (a fraction of the ink
    count) of the other pixels in the same box may have it, which keeps a solid block of that colour from matching."""
    img = image(shot)
    h, w = img.shape[:2]
    x0, y0, rw, rh = region if region else (0, 0, w, h)
    x1, y1 = min(w, x0 + rw), min(h, y0 + rh)
    near = (np.abs(img[y0:y1, x0:x1] - np.array(color, dtype=np.int16)).max(axis=2) <= tol)
    mask = text_mask(text)
    mh, mw = mask.shape
    if near.shape[0] < mh or near.shape[1] < mw:
        return None
    win = np.lib.stride_tricks.sliding_window_view(near, (mh, mw))            # (rows, cols, mh, mw)
    inked = (win & mask).sum(axis=(2, 3))
    total = int(mask.sum())
    stray = (win & ~mask).sum(axis=(2, 3))
    ok = (inked >= ink * total) & (stray <= clean * total + 2)
    ys, xs = np.nonzero(ok)
    if len(xs) == 0:
        return None
    return int(xs[0]) + x0, int(ys[0]) + y0


def count_color(shot, color, region=None, tol=24):
    img = image(shot)
    h, w = img.shape[:2]
    x0, y0, rw, rh = region if region else (0, 0, w, h)
    sub = img[y0:min(h, y0 + rh), x0:min(w, x0 + rw)]
    return int((np.abs(sub - np.array(color, dtype=np.int16)).max(axis=2) <= tol).sum())
