#!/usr/bin/env python3
"""ttfmake.py: builds small, valid TrueType and OpenType font files for the libttf tests, so that the repository needs no third-party font.

Everything is written from the OpenType specification (sfnt directory and checksums, head, hhea, maxp, hmtx, loca, glyf, cmap formats 0/4/6/12/13/14, name,
OS/2, post, kern, GPOS pair adjustment, GSUB ligatures, TrueType collections) and from Adobe Technical Notes 5176/5177 (CFF, Type 2 charstrings). The
builders are deliberately independent of user/lib/ttf: they do not share a line with it, and for each glyph they know the outline the decoder must
produce (`Font.path(gid)`), computed by the plain definition of the format (the TrueType implied-midpoint rule, composite transforms), so a test can
compare a decoded outline with what was put in.

    f = Font(upem=1000)
    f.glyph(".notdef", 500, [box])
    f.glyph("A", 650, [[(0, 0), (300, 700), (600, 0)]])
    f.cmap_bmp({0x41: "A"})
    open("test.ttf", "wb").write(f.build())

    python tests/host/ttfmake.py OUTDIR      writes a few sample fonts (TrueType, CFF, a collection) for looking at
"""
import math
import struct
import sys
from pathlib import Path

# ----------------------------------------------------------------------------- binary helpers


def checksum(data):
    data = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def pad4(data):
    return data + b"\0" * (-len(data) % 4)


def f2dot14(v):
    q = int(round(v * 16384))
    if not -32768 <= q <= 32767:
        raise ValueError("F2Dot14 out of range: %r" % v)
    return q


def build_sfnt(tables, version=0x00010000):
    """tables: {tag: bytes}. Returns an sfnt file with a sorted directory, correct per-table checksums and head.checkSumAdjustment."""
    tags = sorted(tables)
    n = len(tags)
    entry_sel = int(math.floor(math.log2(n))) if n else 0
    search_range = 16 * (1 << entry_sel)
    header = struct.pack(">IHHHH", version, n, search_range, entry_sel, 16 * n - search_range)
    offset = 12 + 16 * n
    records, blobs = b"", b""
    head_off = None
    for tag in tags:
        data = tables[tag]
        if tag == "head":
            data = data[:8] + b"\0\0\0\0" + data[12:]
            head_off = offset
        records += struct.pack(">4sIII", tag.encode("latin-1"), checksum(data), offset, len(data))
        blobs += pad4(data)
        offset += len(pad4(data))
    font = bytearray(header + records + blobs)
    if head_off is not None:
        struct.pack_into(">I", font, head_off + 8, (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF)
    return bytes(font)


def build_ttc(fonts, version=0x00010000):
    """fonts: [(tables dict, sfnt version)]. A collection whose faces share byte-identical tables."""
    nf = len(fonts)
    dir_sizes = [12 + 16 * len(t) for t, _ in fonts]
    pos = 12 + 4 * nf
    dir_offs = []
    for s in dir_sizes:
        dir_offs.append(pos)
        pos += s
    shared = {}
    out_tables = bytearray()
    face_dirs = []
    for (tables, ver), doff in zip(fonts, dir_offs):
        tags = sorted(tables)
        n = len(tags)
        entry_sel = int(math.floor(math.log2(n)))
        search_range = 16 * (1 << entry_sel)
        d = struct.pack(">IHHHH", ver, n, search_range, entry_sel, 16 * n - search_range)
        for tag in tags:
            data = tables[tag]
            if data not in shared:
                shared[data] = pos + len(out_tables)
                out_tables += pad4(data)
            d += struct.pack(">4sIII", tag.encode("latin-1"), checksum(data), shared[data], len(data))
        face_dirs.append(d)
    head = b"ttcf" + struct.pack(">HHI", 1, 0, nf) + b"".join(struct.pack(">I", o) for o in dir_offs)
    return head + b"".join(face_dirs) + bytes(out_tables)


# ----------------------------------------------------------------------------- TrueType outlines


def on(x, y):
    return (x, y, True)


def off(x, y):
    return (x, y, False)


def norm_contour(c):
    return [(p[0], p[1], p[2] if len(p) > 2 else True) for p in c]


def contour_to_path(pts):
    """The TrueType rule for a closed contour of on/off-curve points -> path operations ('M', x, y) ('L', x, y) ('Q', cx, cy, x, y) ('Z',).
    Start at the first point if it is on the curve, else at the last point if that is, else at the midpoint of the two; an off-curve point
    followed by another off-curve point implies an on-curve point halfway between them."""
    n = len(pts)
    if n < 2:
        return []
    if pts[0][2]:
        start, seq = pts[0][:2], pts[1:]
    elif pts[-1][2]:
        start, seq = pts[-1][:2], pts[:-1]
    else:
        start, seq = ((pts[0][0] + pts[-1][0]) / 2, (pts[0][1] + pts[-1][1]) / 2), pts
    ops = [("M", start[0], start[1])]
    ctrl = None
    for x, y, is_on in seq:
        if is_on:
            if ctrl:
                ops.append(("Q", ctrl[0], ctrl[1], x, y))
                ctrl = None
            else:
                ops.append(("L", x, y))
        elif ctrl:
            ops.append(("Q", ctrl[0], ctrl[1], (ctrl[0] + x) / 2, (ctrl[1] + y) / 2))
            ctrl = (x, y)
        else:
            ctrl = (x, y)
    if ctrl:
        ops.append(("Q", ctrl[0], ctrl[1], start[0], start[1]))
    ops.append(("Z",))
    return ops


class Comp:
    """One component of a composite glyph. `gid` may be a glyph name. Either an offset (dx, dy) or `match=(parent_point, child_point)`.
    scale: None, a number, (sx, sy), or (xx, yx, xy, yy) in the file's order (xscale, scale01, scale10, yscale): x' = xx*x + xy*y, y' = yx*x + yy*y."""

    def __init__(self, gid, dx=0, dy=0, match=None, scale=None, words=None, scaled_offset=False, unscaled_offset=False,
                 use_my_metrics=False, round_xy=False, overlap=False):
        self.gid, self.dx, self.dy, self.match, self.scale, self.words = gid, dx, dy, match, scale, words
        self.scaled_offset, self.unscaled_offset, self.use_my_metrics, self.round_xy, self.overlap = scaled_offset, unscaled_offset, use_my_metrics, round_xy, overlap

    def matrix(self):
        """(xx, yx, xy, yy) as the decoder sees it (quantised to F2Dot14)."""
        s = self.scale
        q = lambda v: f2dot14(v) / 16384.0
        if s is None:
            return (1.0, 0.0, 0.0, 1.0)
        if isinstance(s, (int, float)):
            return (q(s), 0.0, 0.0, q(s))
        if len(s) == 2:
            return (q(s[0]), 0.0, 0.0, q(s[1]))
        return tuple(q(v) for v in s)


def encode_simple(contours, compress=True, instructions=b""):
    pts = [p for c in contours for p in c]
    ends, n = [], 0
    for c in contours:
        n += len(c)
        ends.append(n - 1)
    if not pts:
        return struct.pack(">hhhhh", 0, 0, 0, 0, 0)
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    flags, xb, yb = [], b"", b""
    px = py = 0
    for x, y, is_on in pts:
        f = 1 if is_on else 0
        dx, dy = x - px, y - py
        px, py = x, y
        if compress and dx == 0:
            f |= 16
        elif compress and -255 <= dx <= 255:
            f |= 2 | (16 if dx > 0 else 0)
            xb += struct.pack(">B", abs(dx))
        else:
            xb += struct.pack(">h", dx)
        if compress and dy == 0:
            f |= 32
        elif compress and -255 <= dy <= 255:
            f |= 4 | (32 if dy > 0 else 0)
            yb += struct.pack(">B", abs(dy))
        else:
            yb += struct.pack(">h", dy)
        flags.append(f)
    fb, i = b"", 0
    while i < len(flags):
        j = i
        while compress and j + 1 < len(flags) and flags[j + 1] == flags[i] and j - i < 255:
            j += 1
        if j > i:
            fb += struct.pack(">BB", flags[i] | 8, j - i)
        else:
            fb += struct.pack(">B", flags[i])
        i = j + 1
    head = struct.pack(">hhhhh", len(contours), min(xs), min(ys), max(xs), max(ys))
    return head + b"".join(struct.pack(">H", e) for e in ends) + struct.pack(">H", len(instructions)) + instructions + fb + xb + yb


class Glyph:
    def __init__(self, name, advance, contours=None, components=None, lsb=None, compress=True, instructions=b"", raw=None):
        self.name, self.advance, self.lsb = name, advance, lsb
        self.contours = [norm_contour(c) for c in (contours or [])]
        self.components = components
        self.compress, self.instructions, self.raw = compress, instructions, raw


class Font:
    """A TrueType-flavoured font under construction."""

    def __init__(self, upem=1000, ascent=800, descent=-200, line_gap=100, family="Test Sans", style="Regular"):
        self.upem, self.ascent, self.descent, self.line_gap = upem, ascent, descent, line_gap
        self.family, self.style = family, style
        self.glyphs = []
        self.by_name = {}
        self.cmaps = []                       # (platform, encoding, bytes)
        self.extra = {}                       # additional or replacement raw tables
        self.os2 = {}
        self.post = {}
        self.names_extra = []                 # (platform, encoding, language, name_id, bytes)
        self.mac_style = 0

    # ---- glyphs
    def glyph(self, name, advance, contours=None, components=None, **kw):
        g = Glyph(name, advance, contours, components, **kw)
        self.by_name[name] = len(self.glyphs)
        self.glyphs.append(g)
        return len(self.glyphs) - 1

    def gid(self, g):
        return g if isinstance(g, int) else self.by_name[g]

    def points(self, gid, depth=0):
        """Decoded geometry: a list of contours of (x, y, on) in font units, composites resolved with float arithmetic."""
        g = self.glyphs[self.gid(gid)]
        if g.raw is not None:
            raise ValueError("raw glyph has no model")
        if not g.components:
            return [[(float(x), float(y), o) for x, y, o in c] for c in g.contours]
        result = []
        for comp in g.components:
            xx, yx, xy, yy = comp.matrix()
            child = [[(xx * x + xy * y, yx * x + yy * y, o) for x, y, o in c] for c in self.points(comp.gid, depth + 1)]
            if comp.match is None:
                dx, dy = float(comp.dx), float(comp.dy)
                if comp.scaled_offset and not comp.unscaled_offset:
                    dx, dy = xx * comp.dx + xy * comp.dy, yx * comp.dx + yy * comp.dy
            else:
                flat_parent = [p for c in result for p in c]
                flat_child = [p for c in child for p in c]
                pp, cp = flat_parent[comp.match[0]], flat_child[comp.match[1]]
                dx, dy = pp[0] - cp[0], pp[1] - cp[1]
            result += [[(x + dx, y + dy, o) for x, y, o in c] for c in child]
        return result

    def path(self, gid):
        ops = []
        for c in self.points(gid):
            ops += contour_to_path(c)
        return ops

    def bbox(self, gid):
        pts = [p for c in self.points(gid) for p in c]
        if not pts:
            return (0, 0, 0, 0)
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return (int(math.floor(min(xs))), int(math.floor(min(ys))), int(math.ceil(max(xs))), int(math.ceil(max(ys))))

    # ---- cmap
    def cmap_bmp(self, mapping, platform=3, encoding=1, **kw):
        self.cmaps.append((platform, encoding, cmap_format4({k: self.gid(v) for k, v in mapping.items()}, **kw)))

    def cmap_full(self, mapping, platform=3, encoding=10):
        self.cmaps.append((platform, encoding, cmap_format12({k: self.gid(v) for k, v in mapping.items()})))

    def cmap_raw(self, platform, encoding, data):
        self.cmaps.append((platform, encoding, data))

    # ---- table assembly
    def encode_glyph(self, g):
        if g.raw is not None:
            return g.raw
        if g.components:
            out, comps = b"", g.components
            xs, ys = [], []
            b = self.bbox(g.name)
            out += struct.pack(">hhhhh", -1, *b)
            for i, c in enumerate(comps):
                last = i == len(comps) - 1
                xy = c.match is None
                args = (c.dx, c.dy) if xy else c.match
                lo, hi = (-128, 127) if xy else (0, 255)
                words = c.words if c.words is not None else not all(lo <= a <= hi for a in args)
                fl = (1 if words else 0) | (2 if xy else 0) | (4 if c.round_xy else 0) | (0 if last else 0x20)
                fl |= 0x200 if c.use_my_metrics else 0
                fl |= 0x400 if c.overlap else 0
                fl |= 0x800 if c.scaled_offset else 0
                fl |= 0x1000 if c.unscaled_offset else 0
                if last and g.instructions:
                    fl |= 0x100
                s = c.scale
                body = b""
                if s is not None:
                    if isinstance(s, (int, float)):
                        fl |= 0x08
                        body = struct.pack(">h", f2dot14(s))
                    elif len(s) == 2:
                        fl |= 0x40
                        body = struct.pack(">hh", f2dot14(s[0]), f2dot14(s[1]))
                    else:
                        fl |= 0x80
                        body = struct.pack(">hhhh", *(f2dot14(v) for v in s))
                out += struct.pack(">HH", fl, self.gid(c.gid))
                if words:
                    out += struct.pack(">hh" if xy else ">HH", *args)
                else:
                    out += struct.pack(">bb" if xy else ">BB", *args)
                out += body
            if g.instructions:
                out += struct.pack(">H", len(g.instructions)) + g.instructions
            return out
        return encode_simple(g.contours, g.compress, g.instructions)

    def build_tt_tables(self, loca_long=None, compact_hmtx=False, os2_version=4):
        gl = [self.encode_glyph(g) if (g.contours or g.components or g.raw is not None) else b"" for g in self.glyphs]
        glyf, offsets = b"", []
        for data in gl:
            offsets.append(len(glyf))
            glyf += pad4(data)
        offsets.append(len(glyf))
        if loca_long is None:
            loca_long = len(glyf) > 0x1FFFE
        loca = b"".join(struct.pack(">I", o) for o in offsets) if loca_long else b"".join(struct.pack(">H", o // 2) for o in offsets)
        t = {"glyf": glyf, "loca": loca}
        t.update(self.common_tables(loca_long=1 if loca_long else 0, compact_hmtx=compact_hmtx, os2_version=os2_version, maxp_v1=True))
        return t

    def common_tables(self, loca_long, compact_hmtx, os2_version, maxp_v1):
        n = len(self.glyphs)
        boxes = [self.bbox(g.name) if (g.raw is None and (g.contours or g.components)) else (0, 0, 0, 0) for g in self.glyphs]
        nonempty = [b for b, g in zip(boxes, self.glyphs) if g.raw is None and (g.contours or g.components)]
        gb = (min(b[0] for b in nonempty), min(b[1] for b in nonempty), max(b[2] for b in nonempty), max(b[3] for b in nonempty)) if nonempty else (0, 0, 0, 0)
        head = struct.pack(">IIIIHHQQhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B, self.upem, 3600000000, 3600000000,
                           gb[0], gb[1], gb[2], gb[3], self.mac_style, 8, 2, loca_long, 0)
        advs = [g.advance for g in self.glyphs]
        nlong = n
        if compact_hmtx:
            while nlong > 1 and advs[nlong - 1] == advs[nlong - 2]:
                nlong -= 1
        lsbs = [g.lsb if g.lsb is not None else boxes[i][0] for i, g in enumerate(self.glyphs)]
        hmtx = b"".join(struct.pack(">Hh", advs[i], lsbs[i]) for i in range(nlong)) + b"".join(struct.pack(">h", lsbs[i]) for i in range(nlong, n))
        hhea = struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, self.ascent, self.descent, self.line_gap, max(advs), min(lsbs), 0, gb[2], 1, 0, 0, 0, 0, 0, 0, 0, nlong)
        if maxp_v1:
            maxp = struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, n, 64, 8, 64, 8, 2, 0, 0, 0, 0, 0, 0, 0, 4)
        else:
            maxp = struct.pack(">IH", 0x00005000, n)
        return {"head": head, "hhea": hhea, "maxp": maxp, "hmtx": hmtx, "name": self.name_table(), "OS/2": self.os2_table(os2_version),
                "post": self.post_table(), "cmap": cmap_table(self.cmaps)}

    def os2_table(self, version):
        o = dict(weight=400, width=5, fs_selection=0x40, typo_asc=self.ascent, typo_desc=self.descent, typo_gap=self.line_gap,
                 win_asc=self.ascent, win_desc=-self.descent, strike_size=50, strike_pos=300, x_height=500, cap_height=700)
        o.update(self.os2)
        t = struct.pack(">HhHHH8hhhh10sIIII4sHHHhhhHHIIhhHHH", version, 600, o["weight"], o["width"], 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        o["strike_size"], o["strike_pos"], 0, b"\0" * 10, 0, 0, 0, 0, b"TEST", o["fs_selection"], 0x20, 0xFFFF,
                        o["typo_asc"], o["typo_desc"], o["typo_gap"], o["win_asc"], o["win_desc"], 0, 0, o["x_height"], o["cap_height"], 0, 0x20, 0)
        return t

    def post_table(self):
        p = dict(angle=0, pos=-100, thick=50, fixed=0)
        p.update(self.post)
        return struct.pack(">IihhIIIII", 0x00030000, p["angle"] * 65536, p["pos"], p["thick"], p["fixed"], 0, 0, 0, 0)

    def name_table(self):
        recs = []  # (platform, encoding, language, name_id, bytes)
        for nid, text in ((0, "Test copyright"), (1, self.family), (2, self.style), (3, "TestUnique-1"), (4, self.family + " " + self.style),
                          (5, "Version 1.0"), (6, self.family.replace(" ", "") + "-" + self.style.replace(" ", ""))):
            recs.append((3, 1, 0x409, nid, text.encode("utf-16-be")))
            recs.append((1, 0, 0, nid, text.encode("ascii")))
        recs += self.names_extra
        recs.sort(key=lambda r: r[:4])
        strings, rec_bytes, off = b"", b"", 0
        for p, e, l, nid, data in recs:
            rec_bytes += struct.pack(">HHHHHH", p, e, l, nid, len(data), off)
            strings += data
            off += len(data)
        return struct.pack(">HHH", 0, len(recs), 6 + 12 * len(recs)) + rec_bytes + strings

    def build(self, loca_long=None, compact_hmtx=False, os2_version=4, tables=None):
        t = self.build_tt_tables(loca_long, compact_hmtx, os2_version)
        t.update(self.extra)
        if tables:
            t.update(tables)
        return build_sfnt({k: v for k, v in t.items() if v is not None})


# ----------------------------------------------------------------------------- cmap


def cmap_table(subtables):
    """subtables: [(platform, encoding, bytes)] -> a cmap table; identical bodies are stored once."""
    subtables = sorted(subtables, key=lambda s: (s[0], s[1]))
    head = struct.pack(">HH", 0, len(subtables))
    off = 4 + 8 * len(subtables)
    recs, body, where = b"", b"", {}
    for p, e, data in subtables:
        if data not in where:
            where[data] = off + len(body)
            body += pad4(data)
        recs += struct.pack(">HHI", p, e, where[data])
    return head + recs + body


def cmap_format0(mapping):
    arr = bytearray(256)
    for cp, g in mapping.items():
        arr[cp] = g
    return struct.pack(">HHH", 0, 262, 0) + bytes(arr)


def cmap_format6(first, glyphs):
    return struct.pack(">HHHHH", 6, 10 + 2 * len(glyphs), 0, first, len(glyphs)) + b"".join(struct.pack(">H", g) for g in glyphs)


def cmap_format4(mapping, force_array=False):
    """Segments with idDelta where glyphs run in step with the code points, glyphIdArray (idRangeOffset) elsewhere (or everywhere with force_array)."""
    items = sorted(mapping.items())
    runs = []  # (start, end, [glyphs])
    for cp, g in items:
        if runs and cp == runs[-1][1] + 1:
            runs[-1][1] = cp
            runs[-1][2].append(g)
        else:
            runs.append([cp, cp, [g]])
    segs = []   # (start, end, delta, array or None)
    for s, e, gl in runs:
        if not force_array:
            # split a run into stretches where gid - cp is constant (delta) and stretches that need the array
            i = 0
            while i < len(gl):
                j = i
                while j + 1 < len(gl) and gl[j + 1] - gl[j] == 1:
                    j += 1
                segs.append((s + i, s + j, (gl[i] - (s + i)) & 0xFFFF, None))
                i = j + 1
        else:
            # glyphIdArray holds glyph + 1 and idDelta is -1: a stored 0 means "no glyph" whatever the delta says, so no real glyph may be stored as 0
            segs.append((s, e, 0xFFFF, [(g + 1) & 0xFFFF if g else 0 for g in gl]))
    segs.append((0xFFFF, 0xFFFF, 1, None))
    n = len(segs)
    arr, range_offsets = [], []
    for i, (s, e, d, a) in enumerate(segs):
        if a is None:
            range_offsets.append(0)
        else:
            range_offsets.append(2 * (n - i) + 2 * len(arr))
            arr += a
    entry_sel = int(math.floor(math.log2(n)))
    search_range = 2 * (1 << entry_sel)
    body = b"".join(struct.pack(">H", s[1]) for s in segs) + b"\0\0" + b"".join(struct.pack(">H", s[0]) for s in segs)
    body += b"".join(struct.pack(">H", s[2]) for s in segs) + b"".join(struct.pack(">H", r) for r in range_offsets)
    body += b"".join(struct.pack(">H", a) for a in arr)
    length = 14 + len(body)
    return struct.pack(">HHHHHHH", 4, length, 0, 2 * n, search_range, entry_sel, 2 * n - search_range) + body


def cmap_format12(mapping, fmt=12):
    items = sorted(mapping.items())
    groups = []
    for cp, g in items:
        if groups and cp == groups[-1][1] + 1 and g == groups[-1][2] + (groups[-1][1] - groups[-1][0] + 1) * (1 if fmt == 12 else 0):
            groups[-1][1] = cp
        else:
            groups.append([cp, cp, g])
    body = b"".join(struct.pack(">III", s, e, g) for s, e, g in groups)
    return struct.pack(">HHIII", fmt, 0, 16 + len(body), 0, len(groups)) + body


def cmap_format13(ranges):
    """ranges: [(first, last, glyph)]: every code point in the range maps to the one glyph."""
    body = b"".join(struct.pack(">III", s, e, g) for s, e, g in ranges)
    return struct.pack(">HHIII", 13, 0, 16 + len(body), 0, len(ranges)) + body


def cmap_format14(base, selector):
    """One default-UVS record: (selector, base) is the plain character."""
    default = struct.pack(">I", 1) + bytes([(base >> 16) & 255, (base >> 8) & 255, base & 255, 0])
    rec = bytes([(selector >> 16) & 255, (selector >> 8) & 255, selector & 255]) + struct.pack(">II", 10 + 11, 0)     # after the 10-byte header and the one 11-byte record
    return struct.pack(">HII", 14, 10 + len(rec) + len(default), 1) + rec + default


# ----------------------------------------------------------------------------- kern, GPOS, GSUB


def kern_table(pairs, apple=False, override=False, split=None):
    """pairs: {(left gid, right gid): value}. split=N makes N subtables of consecutive pairs (to test several)."""
    items = sorted(pairs.items())
    chunks = [items] if not split else [items[i::split] for i in range(split)]
    subs = b""
    for chunk in chunks:
        chunk = sorted(chunk)
        n = len(chunk)
        es = int(math.floor(math.log2(n))) if n else 0
        sr = 6 * (1 << es)
        body = struct.pack(">HHHH", n, sr, es, 6 * n - sr) + b"".join(struct.pack(">HHh", l, r, v) for (l, r), v in chunk)
        if apple:
            subs += struct.pack(">IHH", 8 + len(body), 0, 0) + body
        else:
            subs += struct.pack(">HHH", 0, (6 + len(body)) & 0xFFFF, 1 | (8 if override else 0)) + body
    if apple:
        return struct.pack(">II", 0x00010000, len(chunks)) + subs
    return struct.pack(">HH", 0, len(chunks)) + subs


def coverage(glyphs, fmt=1):
    glyphs = sorted(set(glyphs))
    if fmt == 1:
        return struct.pack(">HH", 1, len(glyphs)) + b"".join(struct.pack(">H", g) for g in glyphs)
    ranges, idx = [], 0
    for g in glyphs:
        if ranges and g == ranges[-1][1] + 1:
            ranges[-1][1] = g
        else:
            ranges.append([g, g, idx])
        idx += 1
    return struct.pack(">HH", 2, len(ranges)) + b"".join(struct.pack(">HHH", s, e, i) for s, e, i in ranges)


def class_def(mapping, fmt=2):
    if not mapping:
        return struct.pack(">HHH", 1, 0, 0)
    if fmt == 1:
        lo, hi = min(mapping), max(mapping)
        return struct.pack(">HHH", 1, lo, hi - lo + 1) + b"".join(struct.pack(">H", mapping.get(g, 0)) for g in range(lo, hi + 1))
    ranges = []
    for g in sorted(mapping):
        if ranges and g == ranges[-1][1] + 1 and mapping[g] == ranges[-1][2]:
            ranges[-1][1] = g
        else:
            ranges.append([g, g, mapping[g]])
    return struct.pack(">HH", 2, len(ranges)) + b"".join(struct.pack(">HHH", s, e, c) for s, e, c in ranges)


def value_record(fmt, xadv, xplace=7, yplace=-3, yadv=0):
    out = b""
    if fmt & 1: out += struct.pack(">h", xplace)
    if fmt & 2: out += struct.pack(">h", yplace)
    if fmt & 4: out += struct.pack(">h", xadv)
    if fmt & 8: out += struct.pack(">h", yadv)
    if fmt & 0xF0: out += b"\0\0" * bin(fmt & 0xF0).count("1")
    return out


def pairpos1(pairs, vf1=4, vf2=0, cov_fmt=1):
    """PairPos format 1: pairs {(left, right): xadvance}."""
    lefts = sorted({l for l, _ in pairs})
    sets = []
    for l in lefts:
        rs = sorted(r for (ll, r) in pairs if ll == l)
        sets.append(struct.pack(">H", len(rs)) + b"".join(struct.pack(">H", r) + value_record(vf1, pairs[(l, r)]) + value_record(vf2, 99) for r in rs))
    cov = coverage(lefts, cov_fmt)
    head_len = 10 + 2 * len(lefts)
    off = head_len + len(cov)
    offs = []
    for s in sets:
        offs.append(off)
        off += len(s)
    return struct.pack(">HHHHH", 1, head_len, vf1, vf2, len(lefts)) + b"".join(struct.pack(">H", o) for o in offs) + cov + b"".join(sets)


def pairpos2(firsts, class1, class2, matrix, vf1=4, vf2=0, cov_fmt=1, cd_fmt=2):
    """PairPos format 2: `firsts` the covered first glyphs; class1/class2 {glyph: class}; matrix[c1][c2] = xadvance."""
    cov = coverage(firsts, cov_fmt)
    cd1, cd2 = class_def(class1, cd_fmt), class_def(class2, cd_fmt)
    n1, n2 = len(matrix), len(matrix[0])
    recs = b"".join(value_record(vf1, matrix[a][b]) + value_record(vf2, 99) for a in range(n1) for b in range(n2))
    head_len = 16
    off_cov = head_len + len(recs)
    off_c1 = off_cov + len(cov)
    off_c2 = off_c1 + len(cd1)
    return struct.pack(">HHHHHHHH", 2, off_cov, vf1, vf2, off_c1, off_c2, n1, n2) + recs + cov + cd1 + cd2


def lookup_table(ltype, subtables, flag=0, extension=False):
    """A Lookup: subtable offsets are relative to the lookup. With extension the real subtables are reached through Extension subtables."""
    n = len(subtables)
    head_len = 6 + 2 * n
    if not extension:
        offs, off = [], head_len
        for s in subtables:
            offs.append(off)
            off += len(s)
        return struct.pack(">HHH", ltype, flag, n) + b"".join(struct.pack(">H", o) for o in offs) + b"".join(subtables)
    ext_len = 8
    offs = [head_len + i * ext_len for i in range(n)]
    real_off = head_len + n * ext_len
    exts, reals = b"", b""
    for i, s in enumerate(subtables):
        exts += struct.pack(">HHI", 1, ltype, real_off + len(reals) - offs[i])
        reals += s
    return struct.pack(">HHH", 9 if ltype == 2 else 7, flag, n) + b"".join(struct.pack(">H", o) for o in offs) + exts + reals


def otl_table(scripts, features, lookups):
    """GPOS/GSUB: scripts [(tag, [feature indices])], features [(tag, [lookup indices])], lookups [bytes]. The first script gets a default LangSys."""
    script_tabs = []
    for tag, feats in scripts:
        langsys = struct.pack(">HHH", 0, 0xFFFF, len(feats)) + b"".join(struct.pack(">H", f) for f in feats)
        script_tabs.append(struct.pack(">HH", 4, 0) + langsys)
    sl = struct.pack(">H", len(scripts))
    off = 2 + 6 * len(scripts)
    body = b""
    for (tag, _), st in zip(scripts, script_tabs):
        sl += struct.pack(">4sH", tag.encode(), off + len(body))
        body += st
    sl += body
    feat_tabs = [struct.pack(">HH", 0, len(ls)) + b"".join(struct.pack(">H", l) for l in ls) for _, ls in features]
    fl = struct.pack(">H", len(features))
    off = 2 + 6 * len(features)
    body = b""
    for (tag, _), ft in zip(features, feat_tabs):
        fl += struct.pack(">4sH", tag.encode(), off + len(body))
        body += ft
    fl += body
    ll = struct.pack(">H", len(lookups))
    off = 2 + 2 * len(lookups)
    body = b""
    for lk in lookups:
        ll += struct.pack(">H", off + len(body))
        body += lk
    ll += body
    head_len = 10
    return struct.pack(">IHHH", 0x00010000, head_len, head_len + len(sl), head_len + len(sl) + len(fl)) + sl + fl + ll


def gpos_kern(subtables, extension=False, script="DFLT", extra_scripts=()):
    return otl_table([(script, [0])] + list(extra_scripts), [("kern", [0])], [lookup_table(2, subtables, extension=extension)])


def liga_subtable(ligatures, cov_fmt=1):
    """ligatures: [(first gid, [following gids], ligature gid)] in the order the font tries them."""
    firsts = sorted({l[0] for l in ligatures})
    sets = []
    for f in firsts:
        ligs = [l for l in ligatures if l[0] == f]
        lbs = [struct.pack(">HH", lg, len(rest) + 1) + b"".join(struct.pack(">H", r) for r in rest) for _, rest, lg in ligs]
        head_len = 2 + 2 * len(lbs)
        off, offs = head_len, []
        for lb in lbs:
            offs.append(off)
            off += len(lb)
        sets.append(struct.pack(">H", len(lbs)) + b"".join(struct.pack(">H", o) for o in offs) + b"".join(lbs))
    cov = coverage(firsts, cov_fmt)
    head_len = 6 + 2 * len(firsts)
    off = head_len + len(cov)
    offs = []
    for s in sets:
        offs.append(off)
        off += len(s)
    return struct.pack(">HHH", 1, head_len, len(firsts)) + b"".join(struct.pack(">H", o) for o in offs) + cov + b"".join(sets)


def gsub_liga(subtables, extension=False, script="DFLT"):
    return otl_table([(script, [0])], [("liga", [0])], [lookup_table(4, subtables, extension=extension)])


# ----------------------------------------------------------------------------- CFF


def cff_index(items, offsize=None):
    if not items:
        return struct.pack(">H", 0)
    total = sum(len(i) for i in items) + 1
    if offsize is None:
        offsize = 1 if total < 256 else 2 if total < 65536 else 3 if total < (1 << 24) else 4
    offs, o = [1], 1
    for i in items:
        o += len(i)
        offs.append(o)
    out = struct.pack(">HB", len(items), offsize)
    for v in offs:
        out += v.to_bytes(offsize, "big")
    return out + b"".join(items)


def dict_num(v):
    if -107 <= v <= 107:
        return bytes([v + 139])
    if 108 <= v <= 1131:
        return bytes([247 + ((v - 108) >> 8), (v - 108) & 255])
    if -1131 <= v <= -108:
        return bytes([251 + ((-v - 108) >> 8), (-v - 108) & 255])
    if -32768 <= v <= 32767:
        return b"\x1c" + struct.pack(">h", v)
    return b"\x1d" + struct.pack(">i", v)


def dict_off(v):
    return b"\x1d" + struct.pack(">i", v)           # always five bytes, so a dictionary's size does not depend on its offsets


def cs_num(v):
    """A Type 2 charstring operand."""
    if isinstance(v, float) and v != int(v):
        return b"\xff" + struct.pack(">i", int(round(v * 65536)))
    v = int(v)
    if -107 <= v <= 107:
        return bytes([v + 139])
    if 108 <= v <= 1131:
        return bytes([247 + ((v - 108) >> 8), (v - 108) & 255])
    if -1131 <= v <= -108:
        return bytes([251 + ((-v - 108) >> 8), (-v - 108) & 255])
    return b"\x1c" + struct.pack(">h", v)


CS_OPS = {"hstem": b"\x01", "vstem": b"\x03", "vmoveto": b"\x04", "rlineto": b"\x05", "hlineto": b"\x06", "vlineto": b"\x07", "rrcurveto": b"\x08",
          "callsubr": b"\x0a", "return": b"\x0b", "endchar": b"\x0e", "hstemhm": b"\x12", "hintmask": b"\x13", "cntrmask": b"\x14", "rmoveto": b"\x15",
          "hmoveto": b"\x16", "vstemhm": b"\x17", "rcurveline": b"\x18", "rlinecurve": b"\x19", "vvcurveto": b"\x1a", "hhcurveto": b"\x1b",
          "callgsubr": b"\x1d", "vhcurveto": b"\x1e", "hvcurveto": b"\x1f",
          "and": b"\x0c\x03", "or": b"\x0c\x04", "not": b"\x0c\x05", "abs": b"\x0c\x09", "add": b"\x0c\x0a", "sub": b"\x0c\x0b", "div": b"\x0c\x0c",
          "neg": b"\x0c\x0e", "eq": b"\x0c\x0f", "drop": b"\x0c\x12", "put": b"\x0c\x14", "get": b"\x0c\x15", "ifelse": b"\x0c\x16", "random": b"\x0c\x17",
          "mul": b"\x0c\x18", "sqrt": b"\x0c\x1a", "dup": b"\x0c\x1b", "exch": b"\x0c\x1c", "index": b"\x0c\x1d", "roll": b"\x0c\x1e",
          "hflex": b"\x0c\x22", "flex": b"\x0c\x23", "hflex1": b"\x0c\x24", "flex1": b"\x0c\x25"}


def cs(*items):
    """Assembles a charstring: numbers become operands, strings operator names, bytes pass through ('hintmask' masks are written as bytes)."""
    out = b""
    for it in items:
        if isinstance(it, str):
            out += CS_OPS[it]
        elif isinstance(it, (bytes, bytearray)):
            out += bytes(it)
        else:
            out += cs_num(it)
    return out


class CsPath:
    """An absolute-coordinate path that is assembled into a charstring (choosing between the line and curve operator forms) and also
    remembers the outline the decoder must give back: ('M', x, y) ('L', x, y) ('C', x1, y1, x2, y2, x3, y3) ('Z',)."""

    def __init__(self, width_arg=None, prefer=None):
        self.items = []                    # charstring items
        self.expected = []
        self.x = self.y = 0
        self.width_arg = width_arg         # an explicit width operand before the first stack-clearing operator
        self.prefer = prefer               # force a curve operator form: "rr", "hh", "vv", "hv", "vh"
        self.open = False

    def _first(self):
        if self.width_arg is not None:
            w, self.width_arg = self.width_arg, None
            return [w]
        return []

    def move(self, x, y):
        if self.open:
            self.expected.append(("Z",))
        dx, dy = x - self.x, y - self.y
        if dy == 0 and dx != 0 and self.prefer != "rr":
            self.items += self._first() + [dx, "hmoveto"]
        elif dx == 0 and dy != 0 and self.prefer != "rr":
            self.items += self._first() + [dy, "vmoveto"]
        else:
            self.items += self._first() + [dx, dy, "rmoveto"]
        self.x, self.y = x, y
        self.expected.append(("M", x, y))
        self.open = True

    def line(self, x, y):
        dx, dy = x - self.x, y - self.y
        if dy == 0 and self.prefer != "rr":
            self.items += [dx, "hlineto"]
        elif dx == 0 and self.prefer != "rr":
            self.items += [dy, "vlineto"]
        else:
            self.items += [dx, dy, "rlineto"]
        self.x, self.y = x, y
        self.expected.append(("L", x, y))

    def curve(self, x1, y1, x2, y2, x3, y3):
        x0, y0 = self.x, self.y
        d = (x1 - x0, y1 - y0, x2 - x1, y2 - y1, x3 - x2, y3 - y2)
        p = self.prefer
        if p in (None, "hv") and d[1] == 0 and d[4] == 0:
            self.items += [d[0], d[2], d[3], d[5], "hvcurveto"]
        elif p in (None, "vh") and d[0] == 0 and d[5] == 0:
            self.items += [d[1], d[2], d[3], d[4], "vhcurveto"]
        elif p in (None, "hh") and d[5] == 0:
            self.items += ([d[1]] if d[1] else []) + [d[0], d[2], d[3], d[4], "hhcurveto"]
        elif p in (None, "vv") and d[4] == 0:
            self.items += ([d[0]] if d[0] else []) + [d[1], d[2], d[3], d[5], "vvcurveto"]
        else:
            self.items += list(d) + ["rrcurveto"]
        self.x, self.y = x3, y3
        self.expected.append(("C", x1, y1, x2, y2, x3, y3))

    def end(self):
        if self.open:
            self.expected.append(("Z",))
            self.open = False
        self.items += self._first() + ["endchar"]
        return cs(*self.items)


class CffFont:
    """An OpenType-CFF font: glyphs are (name, advance, charstring bytes, expected outline). CID-keyed when fds is given."""

    def __init__(self, upem=1000, ascent=800, descent=-200, line_gap=100, family="Test CFF", style="Regular"):
        self.upem, self.ascent, self.descent, self.line_gap, self.family, self.style = upem, ascent, descent, line_gap, family, style
        self.glyphs = []
        self.by_name = {}
        self.gsubrs = []
        self.local_subrs = []              # for plain CFF
        self.fds = None                    # CID-keyed: [list of local subr lists, one per font dictionary]
        self.fd_of = {}                    # glyph index -> font dictionary
        self.fdselect_format = 0
        self.cmaps = []
        self.ttf = Font(upem, ascent, descent, line_gap, family, style)     # supplies the shared tables (names, OS/2, ...)

    def glyph(self, name, advance, charstring, expected=None, fd=0):
        self.by_name[name] = len(self.glyphs)
        self.glyphs.append((name, advance, charstring, expected or []))
        self.fd_of[len(self.glyphs) - 1] = fd
        self.ttf.glyph(name, advance)       # placeholder entry keeps hmtx/maxp consistent
        return len(self.glyphs) - 1

    def gid(self, g):
        return g if isinstance(g, int) else self.by_name[g]

    def path(self, gid):
        return self.glyphs[self.gid(gid)][3]

    def cmap_bmp(self, mapping, platform=3, encoding=1, **kw):
        self.cmaps.append((platform, encoding, cmap_format4({k: self.gid(v) for k, v in mapping.items()}, **kw)))

    def cff_table(self):
        cid = self.fds is not None
        n = len(self.glyphs)
        name_idx = cff_index([self.family.replace(" ", "").encode()])
        if cid:                                                       # glyphs are CIDs: the charset maps each glyph to its own number
            strings = cff_index([b"Adobe", b"Identity"])
            charset = b"\0" + b"".join(struct.pack(">H", i) for i in range(1, n))
        else:                                                         # glyph names become custom strings (ids from 391)
            strings = cff_index([g[0].encode() for g in self.glyphs[1:]])
            charset = b"\0" + b"".join(struct.pack(">H", 391 + i) for i in range(n - 1))
        gsubr_idx = cff_index(self.gsubrs)
        cs_idx = cff_index([g[2] for g in self.glyphs])
        header = bytes([1, 0, 4, 4])

        def top_dict(cs_off, priv_size, priv_off, fda_off=0, fds_off=0, charset_off=0):
            d = b""
            if cid:
                d += dict_num(391) + dict_num(392) + dict_num(0) + b"\x0c\x1e"          # ROS: Adobe-Identity-0 (string ids)
            d += dict_num(0) + dict_num(-200) + dict_num(1000) + dict_num(800) + b"\x05"      # FontBBox
            d += dict_off(charset_off) + b"\x0f"
            if cid:
                d += dict_off(fda_off) + b"\x0c\x24" + dict_off(fds_off) + b"\x0c\x25"
            d += dict_off(cs_off) + b"\x11"
            if not cid:
                d += dict_off(priv_size) + dict_off(priv_off) + b"\x12"
            return d

        sizes = len(header) + len(name_idx)
        top_len = len(top_dict(0, 0, 0))
        top_idx_len = len(cff_index([b"\0" * top_len]))
        base = sizes + top_idx_len + len(strings) + len(gsubr_idx)
        cs_off = base
        charset_off = cs_off + len(cs_idx)
        pos = charset_off + len(charset)

        def private(subrs_present):
            d = dict_num(500) + b"\x14" + dict_num(0) + b"\x15"          # defaultWidthX, nominalWidthX
            if subrs_present:
                d += dict_off(len(d) + 6) + b"\x13"                       # Subrs: offset from the start of the Private DICT (its size is known: 6 + operand)
            return d

        if not cid:
            priv = private(bool(self.local_subrs))
            priv_off = pos
            blob = priv + (cff_index(self.local_subrs) if self.local_subrs else b"")
            top = top_dict(cs_off, len(priv), priv_off, charset_off=charset_off)
            tail = charset + blob
        else:
            fd_privs, fd_dicts, blobs = [], [], b""
            fda_base = pos
            # layout: FDArray INDEX, FDSelect, then each Private DICT (+ its local subrs)
            fd_count = len(self.fds)
            # a Font DICT holds only Private (size, offset): its size is fixed
            fd_dict_len = len(dict_off(0)) * 2 + 1
            fda_len = len(cff_index([b"\0" * fd_dict_len] * fd_count))
            fdsel = self.fdselect_bytes()
            fds_off = fda_base + fda_len
            priv_pos = fds_off + len(fdsel)
            privs_blob = b""
            fdicts = []
            for subrs in self.fds:
                p = private(bool(subrs))
                fdicts.append(dict_off(len(p)) + dict_off(priv_pos + len(privs_blob)) + b"\x12")
                privs_blob += p + (cff_index(subrs) if subrs else b"")
            top = top_dict(cs_off, 0, 0, fda_base, fds_off, charset_off)
            tail = charset + cff_index(fdicts) + fdsel + privs_blob
        top_idx = cff_index([top])
        assert len(top_idx) == top_idx_len, (len(top_idx), top_idx_len)
        return header + name_idx + top_idx + strings + gsubr_idx + cs_idx + tail

    def fdselect_bytes(self):
        n = len(self.glyphs)
        if self.fdselect_format == 0:
            return b"\0" + bytes(self.fd_of[i] for i in range(n))
        ranges, prev = [], None
        for i in range(n):
            if self.fd_of[i] != prev:
                ranges.append((i, self.fd_of[i]))
                prev = self.fd_of[i]
        return b"\3" + struct.pack(">H", len(ranges)) + b"".join(struct.pack(">HB", s, f) for s, f in ranges) + struct.pack(">H", n)

    def build(self, tables=None):
        t = self.ttf.common_tables(loca_long=0, compact_hmtx=False, os2_version=4, maxp_v1=False)
        t["cmap"] = cmap_table(self.cmaps)
        # head's bounding box and hmtx lsb come from the (empty) placeholder glyphs: fine, nothing reads them for CFF fonts
        t["CFF "] = self.cff_table()
        if tables:
            t.update(tables)
        return build_sfnt({k: v for k, v in t.items() if v is not None}, version=0x4F54544F)


# ----------------------------------------------------------------------------- ready-made shapes (font units, upem 1000)

BOX = [[(50, 0), (450, 0), (450, 700), (50, 700)], [(100, 50), (100, 650), (400, 650), (400, 50)]]       # outer clockwise-ish, inner the other way
SQUARE = [[(100, 0), (100, 500), (600, 500), (600, 0)]]
TRIANGLE = [[(0, 0), (300, 700), (600, 0)]]


def circle_contour(cx=500, cy=400, r=400, n=8):
    """n off-curve points on the circumscribed polygon: the implied on-curve points make a smooth closed quadratic spline."""
    k = r / math.cos(math.pi / n)
    return [off(round(cx + k * math.cos(2 * math.pi * (i + 0.5) / n)), round(cy + k * math.sin(2 * math.pi * (i + 0.5) / n))) for i in range(n)]


def sample_font():
    """A small Latin-ish font with kerning and a ligature, for looking at and for the CLI tests."""
    f = Font()
    f.glyph(".notdef", 500, BOX)
    f.glyph("space", 250)
    f.glyph("A", 650, TRIANGLE)
    f.glyph("V", 650, [[(0, 700), (300, 0), (600, 700)]])
    f.glyph("o", 600, [circle_contour()])
    f.glyph("i", 300, [[(80, 0), (80, 500), (220, 500), (220, 0)], [(80, 580), (80, 720), (220, 720), (220, 580)]])
    f.glyph("f", 350, [[(60, 0), (60, 500), (260, 500), (260, 600), (60, 600), (60, 700), (340, 700), (340, 400), (200, 400), (200, 0)]])
    f.glyph("fi", 650, [[(60, 0), (60, 500), (260, 500), (260, 600), (60, 600), (60, 700), (340, 700), (340, 0)]])
    f.cmap_bmp({0x20: "space", 0x41: "A", 0x56: "V", 0x6F: "o", 0x69: "i", 0x66: "f"})
    f.extra["kern"] = kern_table({(f.gid("A"), f.gid("V")): -80, (f.gid("V"), f.gid("A")): -80})
    f.extra["GSUB"] = gsub_liga([liga_subtable([(f.gid("f"), [f.gid("i")], f.gid("fi"))])])
    return f


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/ttf-samples")
    out.mkdir(parents=True, exist_ok=True)
    sample = sample_font()
    (out / "testsans.ttf").write_bytes(sample.build())
    c = CffFont()
    p = CsPath()
    p.move(100, 0)
    p.line(500, 0)
    p.curve(600, 0, 600, 100, 600, 200)
    p.line(100, 200)
    c.glyph(".notdef", 500, p.end(), p.expected)
    c.cmap_bmp({0x41: ".notdef"})
    (out / "testcff.otf").write_bytes(c.build())
    print("wrote", out)
