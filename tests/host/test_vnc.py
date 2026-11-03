#!/usr/bin/env python3
"""Host tests for vncd, the VNC server (docs/VNC.md): the Linux build of user/bin/vncd (AddressSanitizer and UBSan) against a fake
window server (tests/host/vnchost.py) and the strict viewer of tests/rfbclient.py. Needs gcc (on Windows: WSL, through
tools/hosttest.py):

    python tools/hosttest.py discover -s tests/host -p "test_vnc.py" -v

What runs here is the whole server: handshakes and authentication, every encoding in several pixel formats against the picture the
fake server holds, CopyRect from move hints (and refusing a false hint), the pointer, desktop size changes, keys and clicks as
records for /dev/vinput, the clipboard, hostile streams, a viewer that stops reading, reconnects. tests/case_vnc.py does the same
against the real desktop in QEMU.
"""
import os
import random
import socket
import struct
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vnchost  # noqa: E402
from vnchost import FakeFairy, Vncd  # noqa: E402
import rfbclient  # noqa: E402
from rfbclient import RFBClient, PixelFormat, RFBRefused, RFBAuthFailed, RFBError, expected_values, diff_summary, XK  # noqa: E402

EV_KEY, EV_REL, EV_ABS = 1, 2, 3
KEY_LSHIFT, KEY_A, KEY_SLASH, KEY_1, KEY_ENTER, KEY_LCTRL, KEY_HOME, KEY_KP5, KEY_F12 = 0x2A, 0x1E, 0x35, 0x02, 0x1C, 0x1D, 0xC7, 0x4C, 0x58
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112


def paint(fairy, seed, w=None, h=None):
    """Something with flat areas, gradients, edges and noise: every tile kind of Hextile and ZRLE gets used."""
    rnd = random.Random(seed)
    w, h = w or fairy.w, h or fairy.h
    fairy.fill(0, 0, w, h, 0x0A0F2A, damage=False)
    for y in range(0, h, 4):                                    # a gradient band
        fairy.fill(0, y, w // 2, min(4, h - y), (y * 255 // h) << 16 | 0x40 << 8 | (255 - y * 255 // h), damage=False)
    for _ in range(25):                                         # windows and buttons
        x, y = rnd.randrange(w - 8), rnd.randrange(h - 8)
        fairy.fill(x, y, rnd.randrange(1, min(90, w - x)), rnd.randrange(1, min(60, h - y)), rnd.getrandbits(24), damage=False)
    for _ in range(3):                                          # text-like two-colour patches
        x, y = rnd.randrange(w - 40), rnd.randrange(h - 20)
        vals = [0xECE6D6 if rnd.random() < 0.7 else 0x101010 for _ in range(40 * 20)]
        fairy.put(x, y, 40, 20, vals, damage=False)
    x, y = rnd.randrange(w - 30), rnd.randrange(h - 30)        # noise: a photo
    fairy.put(x, y, 30, 30, [rnd.getrandbits(24) for _ in range(900)], damage=False)
    with fairy.lock:
        fairy.damage.append((0, 0, fairy.w, fairy.h))
        fairy._maybe_send()


def assert_matches(test, c, fairy, what):
    want = expected_values(c.pf, fairy.rgb_bytes(), fairy.w, fairy.h)
    n, box, ex = diff_summary(c.fb, want, fairy.w, fairy.h)
    test.assertEqual(n, 0, f"{what}: {n} pixels differ in {box}, e.g. {ex}")


class VncTest(unittest.TestCase):
    def setUp(self):
        self.dir = vnchost.workdir()
        self.fairy = None
        self.vncd = None
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        if self.vncd:
            self.vncd.stop()
        if self.fairy:
            self.fairy.close()

    def start(self, w=320, h=240, password=None, **kw):
        fargs = {k: kw.pop(k) for k in ("max_w", "max_h", "scale") if k in kw}
        self.fairy = FakeFairy(self.dir, w, h, **fargs)
        self.vncd = Vncd(self.fairy, password=password, **kw)
        self.assertTrue(self.fairy.subscribed.wait(10), "vncd never subscribed to the frames:\n" + self.vncd.text())
        return self.vncd

    def viewer(self, **kw):
        c = RFBClient("127.0.0.1", self.vncd.port, **kw)
        self.clients.append(c)
        return c

    # ---------------------------------------------------------------------------------------------------- handshake
    def test_handshake_versions_without_password(self):
        """No password and listening on 127.0.0.1: None is offered, in 3.8, 3.7 and 3.3; ServerInit has the screen and a name."""
        self.start()
        for v in ("3.8", "3.7", "3.3"):
            c = self.viewer(version=v)
            self.assertEqual(c.sectype, 1)
            if v != "3.3":
                self.assertEqual(c.offered, [1])
            self.assertEqual((c.width, c.height), (320, 240))
            self.assertIn("TouhouOS", c.name)
            self.assertEqual((c.pf.bpp, c.pf.depth, c.pf.true_colour, c.pf.rshift, c.pf.gshift, c.pf.bshift), (32, 24, True, 16, 8, 0))
            c.update(incremental=False)
            assert_matches(self, c, self.fairy, f"first update, RFB {v}")

    def test_password_accepted_and_refused(self):
        """VNC Authentication: the right password gets in (3.8 and 3.3), a wrong one gets SecurityResult 1 with a reason."""
        self.start(password="secret12")
        c = self.viewer(password="secret12")
        self.assertEqual(c.offered, [2])                            # with a password set, None is not offered, even on 127.0.0.1
        c.update(incremental=False)
        assert_matches(self, c, self.fairy, "after VNC authentication")
        c3 = self.viewer(password="secret12", version="3.3")
        self.assertEqual(c3.sectype, 2)
        with self.assertRaises(RFBAuthFailed) as cm:
            self.viewer(password="secret13")
        self.assertIn("not accepted", cm.exception.reason)
        with self.assertRaises(RFBAuthFailed):
            self.viewer(password="secret13", version="3.7")
        with self.assertRaises(RFBError):                          # no password at all
            self.viewer()
        self.assertIn("wrong VNC password", self.vncd.text())

    def test_password_longer_than_eight_counts_eight(self):
        """Viewers send only the first 8 characters: 'secret12xyz' is 'secret12' (and the file refuses longer ones)."""
        self.start(password="secret12")
        self.viewer(password="secret12xyz")

    def test_listen_all_needs_a_password(self):
        """--listen-all: None is never offered; without a password every viewer is turned away with a reason."""
        self.start(listen_all=True)
        self.assertIn("WARNING: listening on every interface", self.vncd.text())
        with self.assertRaises(RFBRefused) as cm:
            self.viewer()
        self.assertIn("no VNC password is set", cm.exception.reason)
        self.vncd.set_password("hakurei1")                          # read at every connection
        c = self.viewer(password="hakurei1")
        self.assertEqual(c.offered, [2])

    def test_bad_password_files_fail_closed(self):
        """A password file others can read, or one with junk in it, turns every viewer away (it never means 'no password')."""
        self.start(password="secret12")
        os.chmod(self.vncd.pwfile, 0o644)
        with self.assertRaises(RFBRefused) as cm:
            self.viewer(password="secret12")
        self.assertIn("not usable", cm.exception.reason)
        os.chmod(self.vncd.pwfile, 0o600)
        self.vncd.pwfile.write_text("too long a password\n")
        with self.assertRaises(RFBRefused):
            self.viewer(password="too long")
        self.vncd.pwfile.write_text("secret12")                     # no newline: cut short
        with self.assertRaises(RFBRefused):
            self.viewer(password="secret12")
        self.vncd.set_password("secret12")
        self.viewer(password="secret12")
        self.assertIn("others may read or write it", self.vncd.text())

    def test_throttle(self):
        """Three wrong passwords within a minute and the address is turned away (with a reason) even with the right one."""
        self.start(password="secret12", args=("--throttle", "3"))
        for _ in range(3):
            with self.assertRaises(RFBAuthFailed):
                self.viewer(password="nope")
        with self.assertRaises(RFBRefused) as cm:
            self.viewer(password="secret12")
        self.assertIn("too many wrong passwords", cm.exception.reason)
        self.assertEqual(self.vncd.text().count("Hatate looks away"), 1)

    def test_kernel_command_line(self):
        """vnc.* words on the kernel command line: the last one of a name wins, and an option given to vncd beats them all."""
        self.start(password="secret12", limits=False, cmdline="text vnc vnc.throttle=0 vnc.throttle=2 vnc.peraddr=1 vnc.listen=all",
                   args=("--peraddr", "0"))
        self.assertIn("WARNING: listening on every interface", self.vncd.text())
        for _ in range(2):
            with self.assertRaises(RFBAuthFailed):
                self.viewer(password="nope")
        with self.assertRaises(RFBRefused) as cm:                   # vnc.throttle=2 (the second word), not 0
            self.viewer(password="secret12")
        self.assertIn("too many wrong passwords", cm.exception.reason)

    def test_wrong_password_is_answered_slowly(self):
        """The answer to a wrong password waits (--fail-delay; 1 s by default)."""
        self.start(password="secret12", args=("--fail-delay", "700"))
        t0 = time.time()
        with self.assertRaises(RFBAuthFailed):
            self.viewer(password="nope")
        self.assertGreater(time.time() - t0, 0.6)

    def test_exclusive_viewer(self):
        """ClientInit with shared-flag 0 closes the other viewers."""
        self.start()
        a = self.viewer()
        a.update(incremental=False)
        self.viewer(shared=False)
        with self.assertRaises(RFBError):
            a.update(incremental=False, timeout=5)

    # ---------------------------------------------------------------------------------------------------- pictures
    def check_encoding(self, enc, pf=None, seeds=(1, 2, 3)):
        self.start(w=200, h=150)
        c = self.viewer()
        if pf:
            c.set_pixel_format(pf)
        c.set_encodings([enc])
        for s in seeds:
            paint(self.fairy, s)
            c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 200, 150), 200, 150)[0] == 0,
                          incremental=False)
            self.assertTrue(all(r[4] == enc for r in c.rect_log), c.rect_log)
        assert_matches(self, c, self.fairy, enc)
        return c

    def test_raw(self):
        """Raw, native format."""
        self.check_encoding("raw")

    def test_hextile(self):
        """Hextile: solid, two-colour and coloured subrects and raw tiles decode to the picture."""
        self.check_encoding("hextile")

    def test_zrle(self):
        """ZRLE: one zlib stream across many rectangles; every tile kind decodes to the picture."""
        c = self.check_encoding("zrle", seeds=(1, 2, 3, 4, 5))
        self.assertGreater(c.bytes_in, 0)

    def test_formats(self):
        """16-bit 565 big endian, 8-bit BGR233 true colour, 32-bit big endian with the colours high, an 8-bit colour map: Raw, Hextile and ZRLE each."""
        formats = [
            PixelFormat(16, 16, True, True, 31, 63, 31, 11, 5, 0),
            PixelFormat(8, 8, False, True, 7, 7, 3, 0, 3, 6),
            PixelFormat(32, 24, True, True, 255, 255, 255, 24, 16, 8),
            PixelFormat(16, 15, False, True, 31, 31, 31, 10, 5, 0),
            PixelFormat(8, 8, False, False, 0, 0, 0, 0, 0, 0),
        ]
        for pf in formats:
            for enc in ("raw", "hextile", "zrle"):
                with self.subTest(pf=repr(pf), enc=enc):
                    self.check_encoding(enc, pf, seeds=(7,))
                    self.tearDown()
                    self.setUp()

    def test_bad_pixel_formats_close(self):
        """Impossible pixel formats end the connection (24 bpp, max not 2^n-1, overlapping colours, a colour map at 32 bpp)."""
        self.start()
        bad = [PixelFormat(24, 24, False, True, 255, 255, 255, 16, 8, 0), PixelFormat(32, 24, False, True, 200, 255, 255, 16, 8, 0),
               PixelFormat(32, 24, False, True, 255, 255, 255, 8, 8, 0), PixelFormat(32, 24, False, False, 0, 0, 0, 0, 0, 0),
               PixelFormat(16, 24, False, True, 31, 63, 31, 11, 5, 0), PixelFormat(16, 16, False, True, 31, 63, 31, 12, 5, 0)]
        for pf in bad:
            c = self.viewer()
            c.set_pixel_format(pf)
            with self.assertRaises(RFBError, msg=repr(pf)):
                c.update(incremental=False, timeout=5)
        self.viewer().update(incremental=False)

    def test_incremental_update_is_small(self):
        """After the first full update a change of one rectangle sends about that rectangle, not the screen."""
        self.start()
        c = self.viewer()
        c.set_encodings(["raw"])
        c.update(incremental=False)
        before = c.bytes_in
        self.fairy.fill(100, 50, 20, 10, 0xFF0000)
        c.wait_update(lambda cl: cl.rgb_at(105, 55) == (255, 0, 0))
        self.assertLess(c.bytes_in - before, 20 * 10 * 4 * 3 + 200)
        assert_matches(self, c, self.fairy, "incremental")

    def test_partial_request_area(self):
        """A request for part of the screen gets changes there; the rest arrives when asked for."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        self.fairy.fill(10, 10, 10, 10, 0x00FF00)
        self.fairy.fill(250, 200, 10, 10, 0x0000FF)
        c.update(incremental=True, area=(0, 0, 100, 100))
        self.assertTrue(all(x + w <= 100 and y + h <= 100 for x, y, w, h, e in c.rect_log), c.rect_log)
        self.assertEqual(c.rgb_at(15, 15), (0, 255, 0))
        c.wait_update(lambda cl: cl.rgb_at(255, 205) == (0, 0, 255))

    # ---------------------------------------------------------------------------------------------------- CopyRect
    def test_copyrect_window_move(self):
        """A window that moves (with the hint) arrives as CopyRect plus the uncovered strip, and the picture is right."""
        self.start()
        c = self.viewer()
        c.set_encodings(["copyrect", "zrle", "cursor"])
        paint(self.fairy, 11)
        c.update(incremental=False)
        assert_matches(self, c, self.fairy, "before the move")
        for dx, dy in ((15, 7), (-9, 12), (0, -20), (25, 0)):
            self.fairy.move(60, 60, 80, 50, dx, dy)
            c.update(incremental=True)
            self.assertIn("copyrect", [r[4] for r in c.rect_log], c.rect_log)
            copied = sum(w * h for x, y, w, h, e in c.rect_log if e == "copyrect")
            self.assertGreaterEqual(copied, 80 * 50 * 0.9, c.rect_log)
            c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)
            # put it back for the next round
            self.fairy.move(60 + dx, 60 + dy, 80, 50, -dx, -dy)
            c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)

    def test_copyrect_slow_viewer_composes_moves(self):
        """Several moves (each its own frame, fetched for a fast viewer) before a slow viewer asks again: the slow one gets one
        CopyRect with the summed shift; both end with the right picture."""
        self.start()
        c = self.viewer()
        c.set_encodings(["copyrect", "hextile", "cursor"])
        fast = self.viewer()
        fast.set_encodings(["copyrect", "raw", "cursor"])
        paint(self.fairy, 12)
        c.update(incremental=False)
        fast.update(incremental=False)
        x, y = 50, 40
        for dx, dy in ((5, 3), (6, 2), (4, 4), (7, 1)):
            self.fairy.move(x, y, 70, 60, dx, dy)
            x += dx
            y += dy
            fast.update(incremental=True)                        # vncd fetches each frame for this one
            self.assertIn("copyrect", [r[4] for r in fast.rect_log], fast.rect_log)
        c.update(incremental=True)
        copies = [(cx, cy, cw, ch) for cx, cy, cw, ch, e in c.rect_log if e == "copyrect"]
        self.assertTrue(copies, c.rect_log)
        self.assertGreaterEqual(sum(w * h for w, h in ((r[2], r[3]) for r in copies)), 70 * 60 * 0.9, c.rect_log)
        for v in (c, fast):
            v.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)

    def test_moves_between_frames_combine(self):
        """Moves the window server gathers into one frame come as one hint from the place of the last frame (fairyd does so; the fake
        here too), and the viewer gets one CopyRect."""
        self.start()
        c = self.viewer()
        c.set_encodings(["copyrect", "zrle", "cursor"])
        paint(self.fairy, 15)
        c.update(incremental=False)
        x, y = 50, 40
        for dx, dy in ((5, 3), (6, 2), (4, 4)):
            self.fairy.move(x, y, 70, 60, dx, dy)
            x += dx
            y += dy
        c.update(incremental=True)
        self.assertIn("copyrect", [r[4] for r in c.rect_log], c.rect_log)
        c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)

    def test_scroll_becomes_copyrect(self):
        """A rectangle whose rows moved up (a terminal printing, no hint from the window server) arrives as CopyRect of the kept rows
        plus the new rows; blank areas do not fool it; a viewer without CopyRect gets pixels; both end with the right picture."""
        self.start()
        rnd = random.Random(5)
        c = self.viewer()
        c.set_encodings(["copyrect", "zrle", "cursor"])
        plain = self.viewer()
        plain.set_encodings(["zrle", "cursor"])
        self.fairy.put(20, 20, 200, 160, [rnd.getrandbits(24) for _ in range(200 * 160)])
        self.fairy.fill(20, 140, 200, 40, 0x101010)                  # a blank area at the bottom, like an empty terminal
        c.update(incremental=False)
        plain.update(incremental=False)
        for lines in (16, 32, 16):
            self.fairy.scroll(20, 20, 200, 160, lines, [rnd.getrandbits(24) for _ in range(200 * lines)])
            c.update(incremental=True)
            copies = [r for r in c.rect_log if r[4] == "copyrect"]
            self.assertTrue(copies, c.rect_log)
            self.assertGreaterEqual(sum(r[2] * r[3] for r in copies), 200 * (160 - lines - 40) * 0.9, c.rect_log)
            for v in (c, plain):
                v.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)
            self.assertNotIn("copyrect", [r[4] for r in plain.rect_log])

    def test_false_move_hint_is_not_trusted(self):
        """A move hint whose pixels do not match is ignored: no CopyRect, and the picture is still right."""
        self.start()
        c = self.viewer()
        c.set_encodings(["copyrect", "raw", "cursor"])
        paint(self.fairy, 13)
        c.update(incremental=False)
        with self.fairy.lock:
            self.fairy.move(60, 60, 80, 50, 10, 10)
        self.fairy.fill(100, 80, 5, 5, 0x123456)                  # after the move, inside the moved window: the hint is false now
        # the two changes may come in one frame or two; in either case the picture must end right
        c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)
        with self.fairy.lock:                                    # a hint that lies outright
            self.fairy.moves.append((10, 10, 40, 40, 3, 3))
            self.fairy.damage.append((10, 10, 43, 43))
            self.fairy.fill(10, 10, 43, 43, 0x654321, damage=False)
            self.fairy._maybe_send()
        c.update(incremental=True)
        self.assertNotIn("copyrect", [r[4] for r in c.rect_log])
        c.wait_update(lambda cl: cl.rgb_at(20, 20) == (0x65, 0x43, 0x21))

    def test_copyrect_with_drawn_pointer(self):
        """A viewer without the Cursor encoding has the pointer drawn into its pixels; a move under the pointer stays right."""
        self.start()
        arrow = [0xFFFFFFFF if (i % 11) <= (i // 11) else 0 for i in range(11 * 16)]
        self.fairy.set_cursor(11, 16, 0, 0, arrow)
        self.fairy.set_pointer(100, 80)
        c = self.viewer()
        c.set_encodings(["copyrect", "raw"])
        paint(self.fairy, 14)
        time.sleep(0.2)
        c.update(incremental=False)
        for _ in range(3):
            self.fairy.move(60, 60, 80, 50, 6, 4)
            self.fairy.move(66, 64, 80, 50, -6, -4)
        time.sleep(0.2)

        def right(cl):
            want = expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240)
            for j in range(16):
                for i in range(11):
                    if arrow[j * 11 + i]:
                        want[(80 + j) * 320 + 100 + i] = cl.pf.from_rgb(255, 255, 255)
            return diff_summary(cl.fb, want, 320, 240)[0] == 0
        c.wait_update(right)

    # ---------------------------------------------------------------------------------------------------- the pointer
    def test_cursor_shape_and_overlay(self):
        """With the Cursor encoding the viewer gets the shape (pixels, mask, hot spot) and clean pixels; without it, the pointer in the pixels."""
        self.start()
        shape = []
        for j in range(20):
            for i in range(12):
                shape.append(0xFF000000 if i in (0, 11) or j in (0, 19) else 0xFFFFFFFF if i < j else 0)
        self.fairy.set_cursor(12, 20, 2, 3, shape)
        self.fairy.set_pointer(50, 60)
        a = self.viewer()
        a.set_encodings(["raw", "cursor"])
        a.wait_update(lambda cl: cl.cursor is not None and cl.cursor[2] == 12, incremental=False)
        hx, hy, w, h, px, mask = a.cursor
        self.assertEqual((hx, hy, w, h), (2, 3, 12, 20))
        for j in range(20):
            for i in range(12):
                opaque = bool(mask[j * 2 + i // 8] & (0x80 >> (i % 8)))
                self.assertEqual(opaque, bool(shape[j * 12 + i] >> 24), (i, j))
                if opaque:
                    self.assertEqual(a.rgb_of(px[j * 12 + i]), ((shape[j * 12 + i] >> 16) & 255,) * 3)
        a.update(incremental=False)
        assert_matches(self, a, self.fairy, "pixels without the pointer")
        b = self.viewer()
        b.set_encodings(["raw"])
        b.update(incremental=False)
        self.assertEqual(b.rgb_at(50 - 2, 60 - 3), (0, 0, 0))     # the outline of the shape, at the hot spot's offset
        self.assertEqual(b.rgb_at(50 - 2 + 8, 60 - 3 + 15), (255, 255, 255))
        self.fairy.set_pointer(200, 150)                           # moving it: the old place is repaired, the new one drawn
        b.wait_update(lambda cl: cl.rgb_at(198, 147) == (0, 0, 0) and cl.rgb_at(48, 57) == self.fairy_rgb(48, 57))
        self.fairy.set_cursor(0, 0, 0, 0, [])                      # hidden: a transparent pixel for the Cursor viewer
        a.wait_update(lambda cl: cl.cursor is not None and cl.cursor[2] == 1 and cl.cursor[5] == b"\0")

    def fairy_rgb(self, x, y):
        v = self.fairy.pixel(x, y)
        return ((v >> 16) & 255, (v >> 8) & 255, v & 255)

    # ---------------------------------------------------------------------------------------------------- desktop size
    def test_desktop_size_change(self):
        """The window server changes size: DesktopSize and ExtendedDesktopSize viewers follow (full picture at the new size);
        a viewer that announced neither is closed."""
        self.start(w=320, h=240, max_w=400, max_h=300)
        a = self.viewer()
        a.set_encodings(["raw", "desktopsize"])
        a.update(incremental=False)
        b = self.viewer()
        b.set_encodings(["zrle", "extdesktopsize"])
        b.update(incremental=False)                                # the first answer is the screen layout
        self.assertEqual(b.size_events[0][:5], (-308, 0, 0, 320, 240))
        b.update(incremental=False)
        c = self.viewer()
        c.update(incremental=False)
        paint(self.fairy, 21, 400, 300)
        self.fairy.set_mode(400, 300)
        a.wait_update(lambda cl: (cl.width, cl.height) == (400, 300))
        self.assertEqual(a.size_events[-1][:5], (-223, 0, 0, 400, 300))
        a.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 400, 300), 400, 300)[0] == 0, incremental=True)
        b.wait_update(lambda cl: (cl.width, cl.height) == (400, 300))
        self.assertEqual(b.size_events[-1][:5], (-308, 0, 0, 400, 300))
        self.assertEqual(b.size_events[-1][5], [(0, 0, 0, 400, 300, 0)])
        b.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 400, 300), 400, 300)[0] == 0)
        with self.assertRaises(RFBError):
            c.update(incremental=True, timeout=5)
        self.assertIn("cannot follow a change of the desktop size", self.vncd.text())

    def test_set_desktop_size(self):
        """SetDesktopSize: refused (status 1) unless --allow-resize; with it the window server is asked (FM_SET_MODE), the asker gets
        reason 1 and the others reason 2; impossible layouts get status 3."""
        self.start(w=640, h=480, max_w=800, max_h=600)
        a = self.viewer()
        a.set_encodings(["raw", "extdesktopsize"])
        a.update(incremental=False)
        a.set_desktop_size(800, 600)
        a.update(incremental=True)
        self.assertEqual(a.size_events[-1][:5], (-308, 1, 1, 640, 480))
        self.tearDown()
        self.setUp()
        self.start(w=640, h=480, max_w=800, max_h=600, args=("--allow-resize",))
        a = self.viewer()
        a.set_encodings(["raw", "extdesktopsize"])
        a.update(incremental=False)
        b = self.viewer()
        b.set_encodings(["raw", "extdesktopsize"])
        b.update(incremental=False)
        a.set_desktop_size(800, 600, [(0, 0, 0, 800, 600, 0), (1, 0, 0, 10, 10, 0)])   # two screens: no
        a.update(incremental=True)
        self.assertEqual(a.size_events[-1][:3], (-308, 1, 3))
        a.set_desktop_size(5000, 600)                                                   # larger than the framebuffer can be
        a.update(incremental=True)
        self.assertEqual(a.size_events[-1][:3], (-308, 1, 3))
        a.set_desktop_size(800, 600)
        deadline = time.time() + 5
        while not self.fairy.mode_requests and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.fairy.mode_requests, [(800, 600)])
        paint(self.fairy, 22, 800, 600)
        self.fairy.set_mode(800, 600)
        a.wait_update(lambda cl: (cl.width, cl.height) == (800, 600))
        self.assertEqual(a.size_events[-1][:5], (-308, 1, 0, 800, 600))
        b.wait_update(lambda cl: (cl.width, cl.height) == (800, 600))
        self.assertEqual(b.size_events[-1][:5], (-308, 2, 0, 800, 600))

    def test_set_desktop_size_needs_announcement(self):
        """SetDesktopSize from a viewer that never announced ExtendedDesktopSize ends its connection."""
        self.start()
        c = self.viewer()
        c.set_desktop_size(320, 240)
        with self.assertRaises(RFBError):
            c.update(incremental=False, timeout=5)

    # ---------------------------------------------------------------------------------------------------- input
    def wait_events(self, n, timeout=5):
        deadline = time.time() + timeout
        while len(self.vncd.events()) < n and time.time() < deadline:
            time.sleep(0.05)
        return self.vncd.events()

    def test_keys(self):
        """Keysyms become key codes: letters, shifted characters with a shift pressed for them, a viewer's shift lifted for an unshifted
        character, the viewer's key repeat, function keys and the keypad, and keys held at disconnect are released."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        c.tap(ord("a"))
        c.tap(ord("!"))                                            # shift + 1, shift pressed by the server
        c.key(XK["Shift_L"], True)
        c.tap(ord("/"))                                            # the viewer holds shift but wants '/': lifted around it
        c.key(XK["Shift_L"], False)
        c.key(ord("a"), True)
        c.key(ord("a"), True)                                      # the viewer's own repeat
        c.key(ord("a"), False)
        c.tap(XK["Return"])
        c.tap(XK["F12"])
        c.tap(XK["KP_5"])
        c.tap(XK["KP_Home"])                                       # the keypad with Num Lock off on the viewer: Home
        c.tap(0xE9)                                                # é: not on the US keyboard, dropped
        c.key(XK["Control_L"], True)                               # held when the viewer goes
        time.sleep(0.3)
        c.close()
        ev = [e for e in self.wait_events(30) if e[0] == EV_KEY]
        want = [(1, KEY_A, 1), (1, KEY_A, 0),
                (1, KEY_LSHIFT, 1), (1, KEY_1, 1), (1, KEY_LSHIFT, 0), (1, KEY_1, 0),
                (1, KEY_LSHIFT, 1), (1, KEY_LSHIFT, 0), (1, KEY_SLASH, 1), (1, KEY_LSHIFT, 1), (1, KEY_SLASH, 0), (1, KEY_LSHIFT, 0),
                (1, KEY_A, 1), (1, KEY_A, 2), (1, KEY_A, 0),
                (1, KEY_ENTER, 1), (1, KEY_ENTER, 0), (1, KEY_F12, 1), (1, KEY_F12, 0), (1, KEY_KP5, 1), (1, KEY_KP5, 0),
                (1, KEY_HOME, 1), (1, KEY_HOME, 0), (1, KEY_LCTRL, 1), (1, KEY_LCTRL, 0)]
        self.assertEqual(ev, want)
        self.assertIn("keysym 0xe9 has no key", self.vncd.text())

    def test_caps_lock_letters(self):
        """With Caps Lock on in the desktop (the window server says so), 'A' needs no shift and 'a' gets one."""
        self.start()
        self.fairy.mods = 8                                        # MOD_CAPS
        self.fairy.set_pointer(1, 1)
        c = self.viewer()
        c.update(incremental=False)
        time.sleep(0.2)
        c.tap(ord("A"))
        c.tap(ord("a"))
        ev = [e for e in self.wait_events(6) if e[0] == EV_KEY]
        self.assertEqual(ev, [(1, KEY_A, 1), (1, KEY_A, 0), (1, KEY_LSHIFT, 1), (1, KEY_A, 1), (1, KEY_LSHIFT, 0), (1, KEY_A, 0)])

    def test_pointer(self):
        """PointerEvent: absolute positions (clamped to the screen), buttons 1-3 as left/middle/right, 4 and 5 as the wheel."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        c.pointer(10, 20, 0)
        c.pointer(10, 20, 1)
        c.pointer(11, 20, 1)
        c.pointer(11, 20, 0)
        c.pointer(11, 20, 4)
        c.pointer(11, 20, 0)
        c.pointer(11, 20, 2)
        c.pointer(11, 20, 0)
        c.pointer(11, 20, 8)
        c.pointer(11, 20, 0)
        c.pointer(11, 20, 16)
        c.pointer(11, 20, 0)
        c.pointer(5000, 5000, 1)                                   # off the screen: the last pixel; held at disconnect: released
        time.sleep(0.2)
        c.close()
        ev = self.wait_events(20)
        want = [(EV_ABS, 0, 10), (EV_ABS, 1, 20), (EV_KEY, BTN_LEFT, 1), (EV_ABS, 0, 11), (EV_ABS, 1, 20), (EV_KEY, BTN_LEFT, 0),
                (EV_KEY, BTN_RIGHT, 1), (EV_KEY, BTN_RIGHT, 0), (EV_KEY, BTN_MIDDLE, 1), (EV_KEY, BTN_MIDDLE, 0),
                (EV_REL, 8, 1), (EV_REL, 8, -1), (EV_ABS, 0, 319), (EV_ABS, 1, 239), (EV_KEY, BTN_LEFT, 1), (EV_KEY, BTN_LEFT, 0)]
        self.assertEqual(ev, want)

    def test_clipboard(self):
        """ClientCutText (Latin-1) reaches the window server as UTF-8; a clipboard change comes back to every viewer as ServerCutText."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        c.cut_text("Reimu's shrine \xe9t\xe9")
        deadline = time.time() + 5
        while not self.fairy.clip_sets and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.fairy.clip_sets, ["Reimu's shrine été"])
        d = self.viewer()
        d.update(incremental=False)
        self.fairy.set_clipboard("Marisa borrowed it — forever")
        for v in (c, d):
            deadline = time.time() + 5
            while not v.cut_texts and time.time() < deadline:
                try:
                    v.update(incremental=True, timeout=0.5)
                except RFBError:
                    pass
            self.assertEqual(v.cut_texts, ["Marisa borrowed it ? forever"])

    # ---------------------------------------------------------------------------------------------------- robustness
    def test_hostile_streams(self):
        """50 malformed or hostile streams (rfbclient.hostile_streams, plus key presses of odd keysyms and a button storm): vncd stays
        up, sanitizers stay quiet, and a well-behaved viewer still sees the right picture."""
        self.start()
        paint(self.fairy, 31)
        init = b"RFB 003.008\n\x01\x01"
        streams = rfbclient.hostile_streams(320, 240)
        streams += [init + b"\x04\x01\0\0" + struct.pack(">I", k) for k in (0, 0xFFFFFFFF, 0x01FFFFFF, 0xFFFF, 0xFE03)]
        streams += [init + b"\x05" + bytes([0xff]) + struct.pack(">HH", 0xffff, 0xffff), init + b"\x05\x1f" * 3000]
        self.assertGreaterEqual(len(streams), 50)
        for data in streams:
            rfbclient.send_hostile("127.0.0.1", self.vncd.port, data)
            self.assertTrue(self.vncd.alive(), "vncd died on " + repr(data[:60]) + "\n" + self.vncd.text()[-2000:])
        c = self.viewer()
        c.set_encodings(["zrle", "copyrect", "cursor"])
        c.update(incremental=False)
        assert_matches(self, c, self.fairy, "after the hostile streams")
        text = self.vncd.text()
        for what in ("bad ProtocolVersion", "unknown message type", "too many encodings", "pixel format", "longer than a megabyte"):
            self.assertIn(what, text)

    def test_handshake_timeout(self):
        """A connection that says nothing is dropped after the handshake time (3 s here)."""
        self.start()
        s = socket.create_connection(("127.0.0.1", self.vncd.port), timeout=10)
        self.assertEqual(s.recv(12), b"RFB 003.008\n")
        t0 = time.time()
        self.assertEqual(s.recv(10), b"")
        self.assertGreater(time.time() - t0, 2)
        s.close()
        self.assertIn("handshake took too long", self.vncd.text())

    def test_slow_viewer_does_not_stall(self):
        """A viewer that asks for updates and never reads: the window server keeps being served and another viewer keeps getting
        updates; the stuck one is dropped once its output has not moved for the stall time (4 s here)."""
        self.start(w=640, h=480)
        s = socket.create_connection(("127.0.0.1", self.vncd.port), timeout=5)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.recv(12)
        s.sendall(b"RFB 003.008\n")
        s.recv(2)
        s.sendall(b"\x01\x01")
        req = b"\x03\x00" + struct.pack(">HHHH", 0, 0, 640, 480)
        s.sendall(req * 5)
        good = self.viewer()
        good.set_encodings(["raw"])
        good.update(incremental=False)
        t0 = time.time()
        frames0 = self.fairy.frames
        n = 0
        while time.time() - t0 < 6:
            paint(self.fairy, 100 + n)
            try:
                s.sendall(req)
            except OSError:
                pass
            good.update(incremental=True, timeout=5)
            n += 1
        self.assertGreater(self.fairy.frames - frames0, 10)
        assert_matches(self, good, self.fairy, "the good viewer, next to the stuck one")
        deadline = time.time() + 6
        while "stopped reading" not in self.vncd.text() and time.time() < deadline:
            time.sleep(0.1)
        self.assertIn("the viewer stopped reading", self.vncd.text())
        s.close()

    def test_many_reconnects(self):
        """100 connections one after another (half of them dropped in the middle of the handshake): nothing piles up."""
        self.start()
        for i in range(100):
            if i % 2:
                s = socket.create_connection(("127.0.0.1", self.vncd.port), timeout=5)
                s.recv(12)
                s.close()
            else:
                c = RFBClient("127.0.0.1", self.vncd.port)
                c.update(incremental=False)
                c.close()
        c = self.viewer()
        c.update(incremental=False)
        assert_matches(self, c, self.fairy, "after 100 connections")
        self.assertTrue(self.vncd.alive())

    def test_window_server_restart(self):
        """The window server goes away and comes back: viewers stay connected and get the new picture."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        self.fairy.disconnect()
        self.fairy.subscribed.clear()
        time.sleep(0.3)
        self.assertIn("the window server went away", self.vncd.text())
        paint(self.fairy, 41)
        self.assertTrue(self.fairy.subscribed.wait(5), self.vncd.text())
        c.wait_update(lambda cl: diff_summary(cl.fb, expected_values(cl.pf, self.fairy.rgb_bytes(), 320, 240), 320, 240)[0] == 0)

    def test_frame_requests_only_when_watched(self):
        """Nobody asking for updates: vncd asks the window server for nothing (no copying in the compositor)."""
        self.start()
        c = self.viewer()
        c.update(incremental=False)
        time.sleep(0.3)
        n0 = self.fairy.requests
        for i in range(10):
            self.fairy.fill(i * 10, 0, 5, 5, 0xFFFFFF)
            time.sleep(0.05)
        time.sleep(0.3)
        self.assertLessEqual(self.fairy.requests - n0, 1)          # at most the one already outstanding
        c.update(incremental=True)
        self.assertEqual(c.rgb_at(92, 2), (255, 255, 255))

    def test_untrusted_frame_offers(self):
        """A program that pretends to be the window server cannot make vncd read another file: a frame file outside the socket's
        directory or with a wrong name is not used."""
        fairy = FakeFairy(self.dir, 320, 240)
        self.fairy = fairy
        orig = fairy._subscribe

        def evil():
            fairy._send(vnchost.FE_FRAME_READY, struct.pack("<I64sIii", 1, b"/etc/passwd", 4096 + 320 * 240 * 4 + 65536, 320, 240))
        fairy._subscribe = evil
        self.vncd = Vncd(fairy)
        time.sleep(1.5)
        self.assertIn("outside its directory", self.vncd.text())
        fairy._subscribe = orig
        self.assertTrue(self.vncd.alive())


if __name__ == "__main__":
    unittest.main()
