"""A strict RFB (VNC) client for the tests of vncd: it stands for a real viewer, so it checks everything the server sends against
the specification (RFC 6143; the pseudo-encodings after the community specification, rfbproto) and raises RFBError on anything
that a careful viewer could not accept.

    from rfbclient import RFBClient, PixelFormat, keysym
    c = RFBClient("127.0.0.1", 5900, password="secret12")         # handshake, authentication, ClientInit / ServerInit
    c.set_encodings(["zrle", "copyrect", "cursor", "desktopsize"])
    c.update(incremental=False)                                     # one FramebufferUpdate, decoded into c.fb
    rgb = c.rgb_at(10, 10)

The framebuffer `fb` holds the viewer's pixel values (in the pixel format in use); `rgb_at()` and `rgb_bytes()` turn them back into
8-bit colours. Each decoder is written from the specification for these tests, not taken from a viewer: Raw, CopyRect, Hextile,
ZRLE, the Cursor, DesktopSize, ExtendedDesktopSize and LastRect pseudo-encodings, SetColourMapEntries, ServerCutText. DES for
VNC Authentication is here too (FIPS 46-3), with the bit reversal of the key bytes that VNC does.
"""
import socket
import struct
import time
import zlib

ENCODINGS = {"raw": 0, "copyrect": 1, "rre": 2, "hextile": 5, "zlib": 6, "tight": 7, "trle": 15, "zrle": 16,
             "cursor": -239, "desktopsize": -223, "lastrect": -224, "extdesktopsize": -308}
NAMES = {v: k for k, v in ENCODINGS.items()}


class RFBError(Exception):
    """The server did something the specification does not allow (or the connection broke)."""


class RFBRefused(RFBError):
    """The server turned the connection away during the handshake; .reason is what it said."""

    def __init__(self, reason):
        super().__init__(f"refused: {reason}")
        self.reason = reason


class RFBAuthFailed(RFBError):
    def __init__(self, reason):
        super().__init__(f"authentication failed: {reason}")
        self.reason = reason


# ------------------------------------------------------------------------------------------------------------------- DES

_IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
       57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
_FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
       36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
_E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
      16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
_P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
_PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
        63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4]
_PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
        41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
_SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
_S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _perm(v, table, in_bits):
    out = 0
    for t in table:
        out = (out << 1) | ((v >> (in_bits - t)) & 1)
    return out


def des_encrypt(key, block):
    """One 8-byte block with an 8-byte key (FIPS 46-3)."""
    k = int.from_bytes(key, "big")
    b = int.from_bytes(block, "big")
    cd = _perm(k, _PC1, 64)
    c, d = cd >> 28, cd & 0x0FFFFFFF
    subkeys = []
    for s in _SHIFTS:
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFF
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFF
        subkeys.append(_perm((c << 28) | d, _PC2, 56))
    b = _perm(b, _IP, 64)
    left, right = b >> 32, b & 0xFFFFFFFF
    for sk in subkeys:
        x = _perm(right, _E, 32) ^ sk
        f = 0
        for i in range(8):
            six = (x >> (42 - 6 * i)) & 63
            f = (f << 4) | _S[i][((six >> 4) & 2 | (six & 1)) * 16 + ((six >> 1) & 15)]
        left, right = right, left ^ _perm(f, _P, 32)
    return _perm((right << 32) | left, _FP, 64).to_bytes(8, "big")


def vnc_response(password, challenge):
    """RFC 6143 7.2.2: both halves of the challenge encrypted with the password, bits of each key byte reversed."""
    pw = password.encode("latin-1")[:8].ljust(8, b"\0")
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in pw)
    return des_encrypt(key, challenge[:8]) + des_encrypt(key, challenge[8:16])


# ---------------------------------------------------------------------------------------------------------- pixel formats

class PixelFormat:
    def __init__(self, bpp=32, depth=24, big_endian=False, true_colour=True, rmax=255, gmax=255, bmax=255, rshift=16, gshift=8, bshift=0):
        self.bpp, self.depth, self.big_endian, self.true_colour = bpp, depth, big_endian, true_colour
        self.rmax, self.gmax, self.bmax = rmax, gmax, bmax
        self.rshift, self.gshift, self.bshift = rshift, gshift, bshift

    @classmethod
    def unpack(cls, b):
        bpp, depth, be, tc, rmax, gmax, bmax, rs, gs, bs = struct.unpack(">BBBBHHHBBB3x", b)
        return cls(bpp, depth, be != 0, tc != 0, rmax, gmax, bmax, rs, gs, bs)

    def pack(self):
        return struct.pack(">BBBBHHHBBB3x", self.bpp, self.depth, int(self.big_endian), int(self.true_colour), self.rmax, self.gmax,
                           self.bmax, self.rshift, self.gshift, self.bshift)

    @property
    def bytes(self):
        return self.bpp // 8

    def cpixel_bytes(self):
        """ZRLE's CPIXEL (RFC 6143 7.7.6): (size, which slice of the wire pixel)."""
        if self.true_colour and self.bpp == 32 and self.depth <= 24:
            mask = (self.rmax << self.rshift) | (self.gmax << self.gshift) | (self.bmax << self.bshift)
            if not mask & 0xFF000000:
                return 3, (slice(1, 4) if self.big_endian else slice(0, 3))
            if not mask & 0xFF:
                return 3, (slice(0, 3) if self.big_endian else slice(1, 4))
        return self.bytes, slice(0, self.bytes)

    def value(self, raw):
        return int.from_bytes(raw, "big" if self.big_endian else "little")

    def value_c(self, raw3):
        """A CPIXEL back to the pixel value."""
        n, sl = self.cpixel_bytes()
        if n == self.bytes:
            return self.value(raw3)
        full = bytearray(4)
        full[sl] = raw3
        return self.value(bytes(full))

    def from_rgb(self, r, g, b):
        """The value vncd sends for an 8-bit colour: (v * max + 127) // 255 per channel; BGR233 indexes for the colour map."""
        if not self.true_colour:
            return (r >> 5) << 5 | (g >> 5) << 2 | (b >> 6)
        return (((r * self.rmax + 127) // 255) << self.rshift | ((g * self.gmax + 127) // 255) << self.gshift |
                ((b * self.bmax + 127) // 255) << self.bshift)

    def __repr__(self):
        return (f"PixelFormat({self.bpp}bpp depth {self.depth} {'BE' if self.big_endian else 'LE'} "
                f"{'tc' if self.true_colour else 'map'} max {self.rmax},{self.gmax},{self.bmax} shift {self.rshift},{self.gshift},{self.bshift})")


NATIVE = PixelFormat()


# --------------------------------------------------------------------------------------------------------------- the client

class RFBClient:
    def __init__(self, host="127.0.0.1", port=5900, password=None, version="3.8", shared=True, timeout=30, connect=True,
                 strict_hextile=True):
        self.host, self.port, self.password, self.version, self.shared = host, port, password, version, shared
        self.timeout = timeout
        self.strict_hextile = strict_hextile
        self.sock = None
        self.buf = b""
        self.encodings = [0]
        self.zlib = zlib.decompressobj()
        self.colour_map = {}
        self.cursor = None              # (hotx, hoty, w, h, pixel values, mask bytes) as last sent
        self.cut_texts = []
        self.bells = 0
        self.size_events = []           # (encoding, reason, status, w, h, screens)
        self.rect_log = []              # every rectangle of the last update: (x, y, w, h, encoding name)
        self.bytes_in = 0
        if connect:
            self.connect()

    # ------------------------------------------------------------------------------------------------ low level
    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.handshake()

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def recv_exact(self, n, timeout=None):
        deadline = time.time() + (timeout if timeout is not None else self.timeout)
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0:
                raise RFBError(f"timed out waiting for {n} bytes (have {len(self.buf)})")
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(max(65536, n - len(self.buf)))
            except socket.timeout:
                raise RFBError(f"timed out waiting for {n} bytes (have {len(self.buf)})") from None
            except OSError as e:
                raise RFBError(f"connection broken: {e}") from None
            if not data:
                raise RFBError(f"the server closed the connection (wanted {n} bytes, have {len(self.buf)})")
            self.buf += data
            self.bytes_in += len(data)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def u8(self):
        return self.recv_exact(1)[0]

    def u16(self):
        return struct.unpack(">H", self.recv_exact(2))[0]

    def u32(self):
        return struct.unpack(">I", self.recv_exact(4))[0]

    def s32(self):
        return struct.unpack(">i", self.recv_exact(4))[0]

    def send(self, data):
        self.sock.sendall(data)

    def reason(self, limit=4096):
        n = self.u32()
        if n > limit:
            raise RFBError(f"a reason string of {n} bytes")
        return self.recv_exact(n).decode("latin-1")

    # ------------------------------------------------------------------------------------------------ handshake
    def handshake(self):
        v = self.recv_exact(12)
        if v != b"RFB 003.008\n":
            raise RFBError(f"the server's ProtocolVersion is {v!r}, not RFB 003.008")
        minor = {"3.8": 8, "3.7": 7, "3.3": 3}[self.version]
        self.send(f"RFB 003.00{minor}\n".encode())
        if minor == 3:
            st = self.u32()
            if st == 0:
                raise RFBRefused(self.reason())
            if st == 1:
                pass                                            # None: straight to ClientInit, no SecurityResult in 3.3
            elif st == 2:
                self.vnc_auth(minor)
            else:
                raise RFBError(f"security type {st} in 3.3 (only 1 and 2 exist)")
            self.sectype = st
        else:
            n = self.u8()
            if n == 0:
                raise RFBRefused(self.reason())
            types = list(self.recv_exact(n))
            self.offered = types
            if any(t == 0 for t in types):
                raise RFBError(f"security type 0 offered among {types}")
            if self.password is not None and 2 in types:
                st = 2
            elif 1 in types:
                st = 1
            elif 2 in types:
                raise RFBError("the server wants a password and none was given")
            else:
                raise RFBError(f"no security type this client knows: {types}")
            self.sectype = st
            self.send(bytes([st]))
            if st == 2:
                self.vnc_auth(minor)
            elif minor == 8:
                r = self.u32()
                if r != 0:
                    raise RFBError(f"SecurityResult {r} after security type None")
        self.send(bytes([1 if self.shared else 0]))
        w, h = self.u16(), self.u16()
        self.pf = PixelFormat.unpack(self.recv_exact(16))
        self.server_pf = self.pf
        name_len = self.u32()
        if name_len > 4096:
            raise RFBError(f"a desktop name of {name_len} bytes")
        self.name = self.recv_exact(name_len).decode("latin-1")
        if not (1 <= w <= 16384 and 1 <= h <= 16384):
            raise RFBError(f"ServerInit size {w}x{h}")
        if self.pf.bpp not in (8, 16, 32) or not self.pf.true_colour:
            raise RFBError(f"ServerInit pixel format {self.pf}")
        self.width, self.height = w, h
        self.fb = [0] * (w * h)

    def vnc_auth(self, minor):
        challenge = self.recv_exact(16)
        self.challenge = challenge
        self.send(vnc_response(self.password or "", challenge))
        r = self.u32()
        if r == 0:
            return
        if r != 1:
            raise RFBError(f"SecurityResult {r} (only 0 and 1 exist)")
        raise RFBAuthFailed(self.reason() if minor == 8 else "")

    # ------------------------------------------------------------------------------------------------ client messages
    def set_pixel_format(self, pf):
        self.send(b"\0\0\0\0" + pf.pack())
        self.pf = pf
        if pf.true_colour:
            self.colour_map = {}

    def set_encodings(self, names):
        codes = [ENCODINGS[n] if isinstance(n, str) else n for n in names]
        self.encodings = codes
        self.send(struct.pack(">BxH", 2, len(codes)) + b"".join(struct.pack(">i", c) for c in codes))

    def request(self, incremental=True, x=0, y=0, w=None, h=None):
        w = self.width if w is None else w
        h = self.height if h is None else h
        self.send(struct.pack(">BBHHHH", 3, 1 if incremental else 0, x, y, w, h))

    def key(self, sym, down):
        self.send(struct.pack(">BBxxI", 4, 1 if down else 0, sym))

    def tap(self, sym):
        self.key(sym, True)
        self.key(sym, False)

    def type_text(self, text):
        for ch in text:
            self.tap(keysym(ch))

    def pointer(self, x, y, mask=0):
        self.send(struct.pack(">BBHH", 5, mask, x, y))

    def click(self, x, y, button=1):
        self.pointer(x, y, 0)
        self.pointer(x, y, 1 << (button - 1))
        self.pointer(x, y, 0)

    def cut_text(self, text):
        data = text.encode("latin-1")
        self.send(struct.pack(">BxxxI", 6, len(data)) + data)

    def set_desktop_size(self, w, h, screens=None):
        screens = screens if screens is not None else [(0, 0, 0, w, h, 0)]
        msg = struct.pack(">BxHHBx", 251, w, h, len(screens))
        for sid, x, y, sw, sh, fl in screens:
            msg += struct.pack(">IHHHHI", sid, x, y, sw, sh, fl)
        self.send(msg)

    # ------------------------------------------------------------------------------------------------ server messages
    def read_message(self, timeout=None):
        """Reads one message; returns its type (0 update, 1 colour map, 2 bell, 3 cut text)."""
        t = self.recv_exact(1, timeout)[0]
        if t == 0:
            self.read_update()
        elif t == 1:
            self.recv_exact(1)
            first, n = self.u16(), self.u16()
            if self.pf.true_colour:
                raise RFBError("SetColourMapEntries while a true-colour format is in use")
            if first + n > 256:
                raise RFBError(f"SetColourMapEntries {first}+{n} beyond an 8-bit map")
            for i in range(n):
                self.colour_map[first + i] = (self.u16(), self.u16(), self.u16())
        elif t == 2:
            self.bells += 1
        elif t == 3:
            self.recv_exact(3)
            n = self.u32()
            if n > 1 << 20:
                raise RFBError(f"ServerCutText of {n} bytes")
            self.cut_texts.append(self.recv_exact(n).decode("latin-1"))
        else:
            raise RFBError(f"unknown server message type {t}")
        return t

    def update(self, incremental=True, timeout=None, request=True, area=None):
        """Asks for an update (unless request=False) and reads messages until a FramebufferUpdate has been decoded."""
        if request:
            if area:
                self.request(incremental, *area)
            else:
                self.request(incremental)
        while True:
            if self.read_message(timeout) == 0:
                return self.rect_log

    def wait_update(self, pred, timeout=20, incremental=True):
        """Asks for updates until pred(self) holds (it may hold already); returns the number of updates read."""
        deadline = time.time() + timeout
        n = 0
        if pred(self):
            return 0
        while True:
            self.update(incremental, timeout=max(0.5, deadline - time.time()))
            n += 1
            if pred(self):
                return n
            if time.time() > deadline:
                raise RFBError("the condition never held")

    def read_update(self):
        self.recv_exact(1)
        n = self.u16()
        self.rect_log = []
        i = 0
        while n == 0xFFFF or i < n:                              # 0xFFFF: LastRect ends the list
            x, y, w, h, enc = struct.unpack(">HHHHi", self.recv_exact(12))
            i += 1
            name = NAMES.get(enc, str(enc))
            self.rect_log.append((x, y, w, h, name))
            if enc == -224:
                if n != 0xFFFF:
                    raise RFBError("LastRect in an update with a rectangle count")
                break
            if enc not in self.encodings and enc != 0:
                raise RFBError(f"encoding {enc} was not asked for (asked: {self.encodings})")
            if enc in (-223, -308):
                self.read_size(enc, x, y, w, h)
                continue
            if enc == -239:
                self.read_cursor(x, y, w, h)
                continue
            if x + w > self.width or y + h > self.height:
                raise RFBError(f"rectangle {x},{y} {w}x{h} outside the {self.width}x{self.height} framebuffer")
            if w == 0 or h == 0:
                raise RFBError(f"an empty rectangle {x},{y} {w}x{h} ({name})")
            if enc == 0:
                self.read_raw(x, y, w, h)
            elif enc == 1:
                self.read_copyrect(x, y, w, h)
            elif enc == 5:
                self.read_hextile(x, y, w, h)
            elif enc == 16:
                self.read_zrle(x, y, w, h)
            else:
                raise RFBError(f"no decoder for encoding {enc}")

    def _pixels(self, data, count):
        b = self.pf.bytes
        if b == 1:
            return list(data[:count])
        return list(struct.unpack(f"{'>' if self.pf.big_endian else '<'}{count}{'I' if b == 4 else 'H'}", data[:count * b]))

    def _put(self, x, y, w, h, values):
        fb, W = self.fb, self.width
        for j in range(h):
            fb[(y + j) * W + x:(y + j) * W + x + w] = values[j * w:(j + 1) * w]

    def read_raw(self, x, y, w, h):
        data = self.recv_exact(w * h * self.pf.bytes)
        self._put(x, y, w, h, self._pixels(data, w * h))

    def read_copyrect(self, x, y, w, h):
        sx, sy = self.u16(), self.u16()
        if sx + w > self.width or sy + h > self.height:
            raise RFBError(f"CopyRect source {sx},{sy} {w}x{h} outside the framebuffer")
        W = self.width
        rows = [self.fb[(sy + j) * W + sx:(sy + j) * W + sx + w] for j in range(h)]    # read all first: overlap is allowed
        for j in range(h):
            self.fb[(y + j) * W + x:(y + j) * W + x + w] = rows[j]

    def read_hextile(self, x, y, w, h):
        B = self.pf.bytes
        bg = fg = None
        for ty in range(y, y + h, 16):
            th = min(16, y + h - ty)
            for tx in range(x, x + w, 16):
                tw = min(16, x + w - tx)
                se = self.u8()
                if se & ~31:
                    raise RFBError(f"Hextile subencoding byte {se:#x} has undefined bits")
                if se & 1:                                      # Raw
                    self.read_raw(tx, ty, tw, th)
                    if self.strict_hextile:
                        bg = fg = None                          # a careful decoder does not trust colours across a raw tile
                    continue
                if se & 2:
                    bg = self._pixels(self.recv_exact(B), 1)[0]
                if bg is None:
                    raise RFBError("a Hextile tile relies on a background that was never given (or was given before a raw tile)")
                tile = [bg] * (tw * th)
                if se & 4:
                    if se & 16:
                        raise RFBError("Hextile ForegroundSpecified together with SubrectsColoured")
                    fg = self._pixels(self.recv_exact(B), 1)[0]
                nsub = 0
                if se & 8:
                    nsub = self.u8()
                    if nsub == 0:
                        raise RFBError("Hextile AnySubrects with a count of 0")
                elif se & 16:
                    raise RFBError("Hextile SubrectsColoured without AnySubrects")
                for _ in range(nsub):
                    if se & 16:
                        colour = self._pixels(self.recv_exact(B), 1)[0]
                    else:
                        if fg is None:
                            raise RFBError("a Hextile subrect relies on a foreground that was never given")
                        colour = fg
                    xy, wh = self.u8(), self.u8()
                    sx, sy, sw, sh = xy >> 4, xy & 15, (wh >> 4) + 1, (wh & 15) + 1
                    if sx + sw > tw or sy + sh > th:
                        raise RFBError(f"a Hextile subrect {sx},{sy} {sw}x{sh} outside its {tw}x{th} tile")
                    for j in range(sy, sy + sh):
                        tile[j * tw + sx:j * tw + sx + sw] = [colour] * sw
                if se & 16:
                    fg = None if self.strict_hextile else fg
                self._put(tx, ty, tw, th, tile)

    def read_zrle(self, x, y, w, h):
        n = self.u32()
        if n > 64 << 20:
            raise RFBError(f"a ZRLE rectangle of {n} bytes")
        raw = self.recv_exact(n)
        data = self.zlib.decompress(raw)
        if self.zlib.unconsumed_tail:
            raise RFBError("ZRLE data the zlib stream did not take")
        pos = 0
        cb, sl = self.pf.cpixel_bytes()

        def take(k):
            nonlocal pos
            if pos + k > len(data):
                raise RFBError(f"ZRLE data ends inside a tile (need {k} at {pos}, have {len(data)})")
            out = data[pos:pos + k]
            pos += k
            return out

        def cpix():
            return self.pf.value_c(take(cb))

        def runlen():
            total = 1
            while True:
                b = take(1)[0]
                total += b
                if b != 255:
                    return total

        for ty in range(y, y + h, 64):
            th = min(64, y + h - ty)
            for tx in range(x, x + w, 64):
                tw = min(64, x + w - tx)
                n_px = tw * th
                se = take(1)[0]
                if se == 0:
                    tile = [cpix() for _ in range(n_px)]
                elif se == 1:
                    tile = [cpix()] * n_px
                elif 2 <= se <= 16:
                    pal = [cpix() for _ in range(se)]
                    bits = 1 if se == 2 else 2 if se <= 4 else 4
                    tile = []
                    row_bytes = (tw * bits + 7) // 8
                    for _ in range(th):
                        rb = take(row_bytes)
                        for i in range(tw):
                            bit = i * bits
                            idx = (rb[bit // 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)
                            if idx >= se:
                                raise RFBError(f"ZRLE packed palette index {idx} of {se}")
                            tile.append(pal[idx])
                elif se == 128:
                    tile = []
                    while len(tile) < n_px:
                        v = cpix()
                        k = runlen()
                        if len(tile) + k > n_px:
                            raise RFBError("a ZRLE run longer than the tile")
                        tile += [v] * k
                elif se >= 130:
                    ps = se - 128
                    pal = [cpix() for _ in range(ps)]
                    tile = []
                    while len(tile) < n_px:
                        b = take(1)[0]
                        idx, k = b & 127, 1
                        if b & 128:
                            k = runlen()
                        if idx >= ps:
                            raise RFBError(f"ZRLE palette index {idx} of {ps}")
                        if len(tile) + k > n_px:
                            raise RFBError("a ZRLE palette run longer than the tile")
                        tile += [pal[idx]] * k
                else:
                    raise RFBError(f"ZRLE subencoding {se} is not defined")
                self._put(tx, ty, tw, th, tile)
        if pos != len(data):
            raise RFBError(f"{len(data) - pos} bytes of ZRLE data left after the last tile")

    def read_cursor(self, hx, hy, w, h):
        if w and h and (hx >= w or hy >= h):
            raise RFBError(f"cursor hot spot {hx},{hy} outside its {w}x{h} shape")
        if w > 1024 or h > 1024:
            raise RFBError(f"a {w}x{h} cursor")
        px = self._pixels(self.recv_exact(w * h * self.pf.bytes), w * h)
        mask = self.recv_exact(((w + 7) // 8) * h)
        self.cursor = (hx, hy, w, h, px, mask)

    def read_size(self, enc, x, y, w, h):
        screens = []
        if enc == -223:
            if x or y:
                raise RFBError("DesktopSize with a position")
            reason, status = 0, 0
        else:
            reason, status = x, y
            if reason > 2 or status > 3:
                raise RFBError(f"ExtendedDesktopSize reason {reason} status {status}")
            n = self.u8()
            self.recv_exact(3)
            for _ in range(n):
                screens.append(struct.unpack(">IHHHHI", self.recv_exact(16)))
            if n < 1:
                raise RFBError("ExtendedDesktopSize without screens")
        if not (1 <= w <= 16384 and 1 <= h <= 16384):
            raise RFBError(f"a desktop size of {w}x{h}")
        self.size_events.append((enc, reason, status, w, h, screens))
        if (w, h) != (self.width, self.height):
            self.width, self.height = w, h
            self.fb = [0] * (w * h)

    # ------------------------------------------------------------------------------------------------ looking at it
    def rgb_of(self, v):
        pf = self.pf
        if not pf.true_colour:
            if v not in self.colour_map:
                raise RFBError(f"pixel {v} has no colour map entry")
            r, g, b = self.colour_map[v]
            return r >> 8, g >> 8, b >> 8
        r = ((v >> pf.rshift) & pf.rmax) * 255 // pf.rmax
        g = ((v >> pf.gshift) & pf.gmax) * 255 // pf.gmax
        b = ((v >> pf.bshift) & pf.bmax) * 255 // pf.bmax
        return r, g, b

    def rgb_at(self, x, y):
        return self.rgb_of(self.fb[y * self.width + x])

    def rgb_bytes(self):
        """The framebuffer as packed RGB bytes (like the body of a P6 PPM)."""
        if self.pf.bpp == 32 and self.pf.true_colour and (self.pf.rmax, self.pf.gmax, self.pf.bmax) == (255, 255, 255):
            rs, gs, bs = self.pf.rshift, self.pf.gshift, self.pf.bshift
            out = bytearray(3 * len(self.fb))
            for i, v in enumerate(self.fb):
                out[3 * i] = (v >> rs) & 255
                out[3 * i + 1] = (v >> gs) & 255
                out[3 * i + 2] = (v >> bs) & 255
            return bytes(out)
        return b"".join(bytes(self.rgb_of(v)) for v in self.fb)

    def with_cursor(self, ptr_x, ptr_y):
        """The framebuffer values with the cursor shape drawn at the pointer (what a viewer shows)."""
        fb = list(self.fb)
        if not self.cursor:
            return fb
        hx, hy, w, h, px, mask = self.cursor
        stride = (w + 7) // 8
        for j in range(h):
            for i in range(w):
                if mask[j * stride + i // 8] & (0x80 >> (i % 8)):
                    X, Y = ptr_x - hx + i, ptr_y - hy + j
                    if 0 <= X < self.width and 0 <= Y < self.height:
                        fb[Y * self.width + X] = px[j * w + i]
        return fb


def expected_values(pf, rgb, w, h):
    """The pixel values a viewer with format pf must hold for a picture given as packed RGB bytes."""
    out = [0] * (w * h)
    cache = {}
    for i in range(w * h):
        key = rgb[3 * i:3 * i + 3]
        v = cache.get(key)
        if v is None:
            v = cache[key] = pf.from_rgb(key[0], key[1], key[2])
        out[i] = v
    return out


def diff_summary(a, b, w, h, limit=5):
    """How many pixels differ between two value lists, the bounding box, and a few examples."""
    bad = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    if not bad:
        return 0, None, []
    xs = [i % w for i in bad]
    ys = [i // w for i in bad]
    examples = [(i % w, i // w, a[i], b[i]) for i in bad[:limit]]
    return len(bad), (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1), examples


def hostile_streams(w=320, h=240, seed=99):
    """More than fifty byte streams a careful server must survive (sent after reading its ProtocolVersion): garbage, impossible
    versions, wrong security types, impossible pixel formats, lying lengths, unknown messages, storms, cut-off messages. They assume
    a server that offers None (no password) and a framebuffer of w x h."""
    import random
    good_ver = b"RFB 003.008\n"
    init = good_ver + b"\x01" + b"\x01"                        # None, shared
    pf = lambda *b: init + b"\x00" + b"\0" * 3 + bytes(b)      # noqa: E731
    s = [b"", b"\0" * 12, b"GET / HTTP/1.1\r\n\r\n", b"RFB 003.008", b"RFB 003.009\n", b"RFB 004.000\n", b"RFB 003.00a\n",
         b"RFB 002.008\n", b"rfb 003.008\n", b"RFB 003.008\r", b"\xff" * 5000, b"RFB 003.002\n"]
    s += [good_ver + b"\x00", good_ver + b"\x02", good_ver + b"\x7f", good_ver + b"\x01\x01\x01\x01"]
    s += [pf(24, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0)]          # 24 bits per pixel
    s += [pf(32, 0, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0)]           # depth 0
    s += [pf(32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 30, 8, 0, 0, 0, 0)]          # a shift beyond the pixel
    s += [init + b"\x02\x00\xff\xff" + b"\0" * 64]                              # 65535 encodings announced
    s += [init + b"\x02\x00\x02\x00" + b"\x00\x00\x00\x10" + b"\xff\xff\xff\x11"]      # 512 announced, 2 sent
    s += [init + b"\x02\x00\x00\x03" + struct.pack(">iii", 16, -239, 0x7fffffff)]
    s += [init + b"\x03\x00" + struct.pack(">HHHH", 0xffff, 0xffff, 0xffff, 0xffff)]
    s += [init + b"\x03\x01" + struct.pack(">HHHH", w - 20, h - 40, 100, 100) + b"\x03\x00\x00\x00\x00\x00\x00\x00\x00"]
    s += [init + b"\x06\0\0\0" + struct.pack(">I", 0xFFFFFFFF)]
    s += [init + b"\x06\0\0\0" + struct.pack(">I", 2 << 20) + b"x" * 100]
    s += [init + b"\x06\0\0\0" + struct.pack(">I", 5000) + b"y" * 5000 + b"\x04\x01\0\0\0\0\0a"]
    s += [init + b"\x06\0\0\0" + struct.pack(">I", 10) + b"short"]
    s += [init + bytes([t]) + b"\0" * 20 for t in (1, 7, 8, 9, 150, 200, 248, 250, 252, 255)]
    s += [init + b"\xfb\0" + struct.pack(">HHBx", 800, 600, 1) + b"\0" * 16]           # SetDesktopSize unannounced
    s += [init + b"\x04\x00\0\0" + struct.pack(">I", k) for k in (0, 0xFFFFFFFF, 0x01FFFFFF, 0xFFFF, 0xFE03)]   # key releases of odd keysyms
    s += [init + b"\x05" + bytes([0]) + struct.pack(">HH", 0xffff, 0xffff)]
    s += [init + b"\x05\x00" + b"\x01" * 3]
    s += [good_ver + b"\x01" + b"\x00" + b"\x05\x00" * 2000]   # exclusive, then a storm of pointer bytes cut in the middle
    s += [init + (b"\x03\x00" + struct.pack(">HHHH", 0, 0, w, h)) * 200]                # requests and never reading
    rnd = random.Random(seed)
    while len(s) < 52:
        s.append(init + bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(1, 300))))
    return s


def send_hostile(host, port, data, linger=0.3):
    """Sends one hostile stream to a fresh connection and reads whatever comes back for a moment."""
    try:
        s = socket.create_connection((host, port), timeout=10)
    except OSError:
        return
    try:
        s.recv(12)
        s.sendall(data)
        s.settimeout(linger)
        try:
            while s.recv(65536):
                pass
        except (socket.timeout, OSError):
            pass
    except OSError:
        pass
    finally:
        s.close()


# ------------------------------------------------------------------------------------------------------------------- keys

_KEYSYMS = {"\n": 0xFF0D, "\r": 0xFF0D, "\t": 0xFF09, "\b": 0xFF08, "\x1b": 0xFF1B}
XK = {
    "BackSpace": 0xFF08, "Tab": 0xFF09, "Return": 0xFF0D, "Escape": 0xFF1B, "Delete": 0xFFFF, "Home": 0xFF50, "Left": 0xFF51,
    "Up": 0xFF52, "Right": 0xFF53, "Down": 0xFF54, "Page_Up": 0xFF55, "Page_Down": 0xFF56, "End": 0xFF57, "Insert": 0xFF63,
    "Shift_L": 0xFFE1, "Shift_R": 0xFFE2, "Control_L": 0xFFE3, "Control_R": 0xFFE4, "Caps_Lock": 0xFFE5, "Alt_L": 0xFFE9,
    "Alt_R": 0xFFEA, "Super_L": 0xFFEB, "Super_R": 0xFFEC, "Num_Lock": 0xFF7F, "KP_Enter": 0xFF8D, "KP_Add": 0xFFAB,
    "KP_0": 0xFFB0, "KP_1": 0xFFB1, "KP_5": 0xFFB5, "KP_9": 0xFFB9, "KP_Home": 0xFF95, "Menu": 0xFF67,
    **{f"F{i}": 0xFFBD + i for i in range(1, 13)},
}


def keysym(ch):
    """The X11 keysym of a character (Latin-1 directly, other characters as Unicode keysyms)."""
    if ch in _KEYSYMS:
        return _KEYSYMS[ch]
    o = ord(ch)
    return o if 0x20 <= o <= 0xFF else 0x01000000 + o
