#!/usr/bin/env python3
"""Generates shared/font8x16.c: an 8x16 bitmap font for the kernel console and userland GUI.

Glyph sources:
  * ASCII, Latin-1 and a few symbols: DejaVu Sans Mono (Bitstream Vera license, see assets/fonts/LICENSE_DEJAVU)
    rasterised at 13 px with FreeType's monochrome hinting.
  * Box drawing (light/heavy/double) and block elements: drawn procedurally so neighbouring cells join seamlessly.
The output is checked in; this script is only needed to regenerate it.
"""
import os
import re
import sys
import unicodedata
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
W, H, BASELINE, SIZE = 8, 16, 13, 13


def find_font():
    import matplotlib
    return os.path.join(matplotlib.get_data_path(), "fonts", "ttf", "DejaVuSansMono.ttf")


FONT = ImageFont.truetype(find_font(), SIZE)


def render_ttf(cp):
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((0, BASELINE), chr(cp), font=FONT, fill=1, anchor="ls")
    rows = []
    for y in range(H):
        v = 0
        for x in range(W):
            if img.getpixel((x, y)):
                v |= 0x80 >> x
        rows.append(v)
    return rows


# ---------------------------------------------------------------- box drawing

STYLE_WORDS = {"LIGHT": 1, "SINGLE": 1, "HEAVY": 2, "DOUBLE": 3}
DIR_WORDS = {"UP": "u", "DOWN": "d", "LEFT": "l", "RIGHT": "r", "VERTICAL": "ud", "HORIZONTAL": "lr"}


def parse_box_name(cp):
    try:
        name = unicodedata.name(chr(cp))
    except ValueError:
        return None
    if not name.startswith("BOX DRAWINGS "):
        return None
    body = name[len("BOX DRAWINGS "):]
    if "DASH" in body or "ARC" in body or "DIAGONAL" in body:
        return None
    arms = {"u": 0, "d": 0, "l": 0, "r": 0}
    inherited = 0
    for part in body.split(" AND "):
        style = 0
        dirs = ""
        for w in part.split():
            if w in STYLE_WORDS:
                style = STYLE_WORDS[w]
            elif w in DIR_WORDS:
                dirs += DIR_WORDS[w]
            else:
                return None
        if not dirs:
            return None
        if style == 0:
            style = inherited
        if style == 0:
            return None
        inherited = style
        for d in dirs:
            arms[d] = style
    return arms


def box_glyph(arms):
    """Returns 16 row bytes, or None for mixed double/single combinations."""
    styles = {s for s in arms.values() if s}
    px = [[0] * W for _ in range(H)]

    def hline(row, x0, x1):
        for x in range(max(0, x0), min(W - 1, x1) + 1):
            px[row][x] = 1

    def vline(col, y0, y1):
        for y in range(max(0, y0), min(H - 1, y1) + 1):
            px[y][col] = 1

    if 3 in styles:
        if styles != {3}:
            return None
        u, d, l, r = (bool(arms[k]) for k in "udlr")
        for c, side, opp in ((2, l, r), (5, r, l)):
            if u:
                end = 6 if side else (9 if opp else (15 if d else 8))
                vline(c, 0, end)
            if d:
                start = 9 if side else (6 if opp else (0 if u else 7))
                vline(c, start, 15)
        for rr, top, other in ((6, u, d), (9, d, u)):
            if l:
                end = 2 if top else (5 if other else (7 if r else 4))
                hline(rr, 0, end)
            if r:
                start = 5 if top else (2 if other else (0 if l else 3))
                hline(rr, start, 7)
    else:
        def cols(style):
            return (3,) if style == 1 else (3, 4)

        def rows(style):
            return (7,) if style == 1 else (7, 8)

        vcols = sorted({c for k in "ud" if arms[k] for c in cols(arms[k])})
        hrows = sorted({r for k in "lr" if arms[k] for r in rows(arms[k])})
        if arms["u"]:
            hi = max(hrows) if hrows else max(rows(arms["u"]))
            for c in cols(arms["u"]):
                vline(c, 0, hi)
        if arms["d"]:
            lo = min(hrows) if hrows else min(rows(arms["d"]))
            for c in cols(arms["d"]):
                vline(c, lo, 15)
        if arms["l"]:
            hi = max(vcols) if vcols else max(cols(arms["l"]))
            for r in rows(arms["l"]):
                hline(r, 0, hi)
        if arms["r"]:
            lo = min(vcols) if vcols else min(cols(arms["r"]))
            for r in rows(arms["r"]):
                hline(r, lo, 7)
    return [sum(0x80 >> x for x in range(W) if px[y][x]) for y in range(H)]


# ---------------------------------------------------------------- block elements

def block_glyph(cp):
    px = [[0] * W for _ in range(H)]

    def fill(x0, y0, x1, y1):
        for y in range(y0, y1):
            for x in range(x0, x1):
                px[y][x] = 1

    if cp == 0x2580:
        fill(0, 0, 8, 8)
    elif 0x2581 <= cp <= 0x2588:
        n = cp - 0x2580
        fill(0, 16 - 2 * n, 8, 16)
    elif 0x2589 <= cp <= 0x258F:
        n = 0x2590 - cp
        fill(0, 0, n, 16)
    elif cp == 0x2590:
        fill(4, 0, 8, 16)
    elif cp in (0x2591, 0x2592, 0x2593):
        for y in range(H):
            for x in range(W):
                if cp == 0x2591:
                    px[y][x] = 1 if (x % 4 == 0 and y % 4 == 0) or (x % 4 == 2 and y % 4 == 2) else 0
                elif cp == 0x2592:
                    px[y][x] = 1 if (x + y) % 2 == 0 else 0
                else:
                    px[y][x] = 0 if (x % 4 == 0 and y % 4 == 0) or (x % 4 == 2 and y % 4 == 2) else 1
    elif cp == 0x2594:
        fill(0, 0, 8, 2)
    elif cp == 0x2595:
        fill(7, 0, 8, 16)
    elif 0x2596 <= cp <= 0x259F:
        quad = {0x2596: "bl", 0x2597: "br", 0x2598: "tl", 0x2599: "tl bl br", 0x259A: "tl br",
                0x259B: "tl tr bl", 0x259C: "tl tr br", 0x259D: "tr", 0x259E: "tr bl", 0x259F: "tr bl br"}[cp]
        for q in quad.split():
            fill(0 if q[1] == "l" else 4, 0 if q[0] == "t" else 8, 4 if q[1] == "l" else 8, 8 if q[0] == "t" else 16)
    else:
        return None
    return [sum(0x80 >> x for x in range(W) if px[y][x]) for y in range(H)]


def notdef_glyph():
    rows = [0] * H
    for y in range(2, 14):
        rows[y] = 0x42
    rows[2] = rows[13] = 0x7E
    return rows


# ---------------------------------------------------------------- build the table

TTF_RANGES = [(0x20, 0x7E), (0xA0, 0xFF)]
TTF_EXTRA = [0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2195, 0x21B5, 0x2212, 0x2260, 0x2264, 0x2265,
             0x25A0, 0x25A1, 0x25B2, 0x25B6, 0x25BA, 0x25BC, 0x25C0, 0x25C4, 0x25C6, 0x25C7, 0x25CB, 0x25CF,
             0x2605, 0x2606, 0x2610, 0x2611, 0x2612, 0x2713, 0x2717, 0x266A, 0x266B, 0x00D7, 0x2248, 0x221E]


def build():
    glyphs = {}
    for lo, hi in TTF_RANGES:
        for cp in range(lo, hi + 1):
            glyphs[cp] = render_ttf(cp)
    for cp in TTF_EXTRA:
        glyphs[cp] = render_ttf(cp)
    for cp in range(0x2500, 0x2580):
        arms = parse_box_name(cp)
        if arms:
            g = box_glyph(arms)
            if g:
                glyphs[cp] = g
    for cp in range(0x2580, 0x25A0):
        g = block_glyph(cp)
        if g:
            glyphs[cp] = g
    glyphs[0x00A0] = [0] * H
    return dict(sorted(glyphs.items()))


def emit(glyphs):
    ascii_cps = list(range(0x20, 0x7F))
    cps = list(glyphs.keys())
    assert cps[: len(ascii_cps)] == ascii_cps, "ASCII must lead the table"
    out = []
    out.append("/* GENERATED by tools/genfont.py -- do not edit by hand. */")
    out.append('#include "font8x16.h"')
    out.append("")
    out.append("typedef struct { unsigned cp; unsigned char rows[16]; } Glyph;")
    out.append("")
    out.append("static const Glyph glyphs[] = {")
    for cp, rows in glyphs.items():
        label = chr(cp) if 0x20 < cp < 0x7F and chr(cp) not in "\\*/" else ""
        out.append("    { 0x%04X, { %s } },%s" % (cp, ", ".join("0x%02X" % r for r in rows), f" /* {label} */" if label else ""))
    out.append("};")
    out.append("")
    out.append("static const unsigned char notdef[16] = { %s };" % ", ".join("0x%02X" % r for r in notdef_glyph()))
    out.append("")
    out.append("const unsigned char *font8x16_glyph(unsigned cp) {")
    out.append("    if (cp >= 0x20 && cp < 0x7F) return glyphs[cp - 0x20].rows;")
    out.append("    unsigned lo = 0x5F, hi = sizeof(glyphs) / sizeof(glyphs[0]);")
    out.append("    while (lo < hi) {")
    out.append("        unsigned mid = (lo + hi) / 2;")
    out.append("        if (glyphs[mid].cp == cp) return glyphs[mid].rows;")
    out.append("        if (glyphs[mid].cp < cp) lo = mid + 1; else hi = mid;")
    out.append("    }")
    out.append("    return notdef;")
    out.append("}")
    out.append("")
    return "\n".join(out)


def preview(glyphs, path):
    cps = list(glyphs.keys())
    cols = 32
    rowsn = (len(cps) + cols - 1) // cols
    img = Image.new("L", (cols * 9, rowsn * 17), 30)
    for i, cp in enumerate(cps):
        gx, gy = (i % cols) * 9, (i // cols) * 17
        for y in range(H):
            for x in range(W):
                if glyphs[cp][y] & (0x80 >> x):
                    img.putpixel((gx + x, gy + y), 230)
    img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
    img.save(path)


if __name__ == "__main__":
    g = build()
    (ROOT / "shared" / "font8x16.c").write_text(emit(g), newline="\n")
    print(f"font8x16: {len(g)} glyphs")
    if len(sys.argv) > 1:
        preview(g, sys.argv[1])
        print("preview ->", sys.argv[1])
