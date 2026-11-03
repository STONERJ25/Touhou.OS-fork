"""The VNC server (docs/VNC.md): vncd serving the real desktop in QEMU, checked with the strict viewer of tests/rfbclient.py.

Every test boots a root console with "vnc vnc.listen=all" (QEMU's user networking brings the host's connections to the guest's
10.0.2.15, not to 127.0.0.1, so the tests need the deliberate flag; vnc_throttle_and_defaults checks the default), forwards port 5900 to
a port on the host, sets a password with `vncd -passwd` and starts the desktop by hand (fairyd and hakurei-shell, as root). The
viewer's picture is compared with QEMU's own screendump, pixel for pixel: pixels that changed between two screendumps taken around
the update (the panel's meters) are left out, and there may be only a few of them.

    python tools/qemu-slot.py -- python tests/run_all.py vnc_
"""
import re
import socket
import struct
import time

import test_shell
from harness import test, Qemu, BUILD, scaled
from qemu import free_port
from rfbclient import RFBClient, PixelFormat, RFBError, RFBRefused, RFBAuthFailed, XK, expected_values, hostile_streams, send_hostile

ROOT_PROMPT = r"root@gensokyo \S+ # "
PASSWORD = "hatate12"
START_BUTTON = (118, 756)
SHRINE_RED = (0xB5, 0x12, 0x2B)


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


class VncGuest:
    def __init__(self, name, cmdline="", listen_all=True, password=PASSWORD, desktop=True, disk=None, mem="768M"):
        self.port = free_port()
        self.password = password
        self.desktop = desktop
        extra = cmdline.split()
        defaults = [w for w in ("vnc.throttle=0", "vnc.peraddr=0") if not any(e.startswith(w.split("=")[0] + "=") for e in extra)]
        words = ["text", "user=root", "vnc"] + defaults + (["vnc.listen=all"] if listen_all else []) + extra
        self.vm = Qemu(cmdline=" ".join(words), name=name, net="e1000", hostfwd=[f"tcp:127.0.0.1:{self.port}-:5900"], mem=mem, disk=disk)
        self.viewers = []

    def __enter__(self):
        self.vm.start()
        self.vm.expect(ROOT_PROMPT, timeout=240)
        deadline = time.time() + scaled(60)
        while "Hatate keeps watch" not in self.out("dmesg | grep vncd") and "listening on every interface" not in self.out("dmesg | grep vncd"):
            assert time.time() < deadline, "vncd never started:\n" + self.out("dmesg | tail -30")
            time.sleep(1)
        if self.password:
            out, rc = self.run(f"echo {self.password} | vncd -passwd")
            assert rc == 0 and "password is set" in out, out
        if self.desktop:
            self.start_desktop()
        return self

    def __exit__(self, *exc):
        for v in self.viewers:
            v.close()
        self.vm.stop()

    def run(self, cmd, timeout=60):
        return test_shell.run(self.vm, cmd, timeout=timeout, prompt=ROOT_PROMPT)

    def out(self, cmd, timeout=60):
        return self.run(cmd, timeout)[0]

    def start_desktop(self, as_user=None):
        """fairyd and hakurei-shell by hand, as root or (as_user) as that user, the way init's session runs them."""
        wrap = (lambda cmd: f"su {as_user} -c {cmd}") if as_user else (lambda cmd: cmd)
        self.vm.send(wrap("fairyd") + " &\n")
        time.sleep(1.5)
        self.vm.send(wrap("hakurei-shell") + " &\n")
        deadline = time.time() + scaled(60)
        while not near(self.vm.pixel(*START_BUTTON), SHRINE_RED, 60):
            assert time.time() < deadline, "the panel never appeared:\n" + self.vm.text()[-800:]
            time.sleep(0.5)
        deadline = time.time() + scaled(30)
        while "has the desktop in view" not in self.out("dmesg | grep vncd"):
            assert time.time() < deadline, "vncd never subscribed to the frames:\n" + self.out("dmesg | grep -e vncd -e fairyd")
            time.sleep(1)
        time.sleep(1.5)                                          # the shell's first paint settles

    def viewer(self, password=PASSWORD, **kw):
        kw.setdefault("timeout", scaled(60))
        c = RFBClient("127.0.0.1", self.port, password=password, **kw)
        self.viewers.append(c)
        return c

    def log(self):
        return self.out("dmesg | grep vncd")


def compare(g, c, cursor_at=None, tries=6, what="the picture"):
    """Requests a full update and compares the viewer's pixels with QEMU's screendump (see the module docstring). cursor_at: the
    pointer's position, for a viewer that draws the pointer itself (its shape is then put into its pixels first)."""
    last = ""
    for _ in range(tries):
        w, h, a = g.vm.screen_ppm()
        c.update(incremental=False, timeout=scaled(60))
        _, _, b = g.vm.screen_ppm()
        assert (c.width, c.height) == (w, h), f"the viewer has {c.width}x{c.height}, the screen is {w}x{h}"
        fb = c.with_cursor(*cursor_at) if cursor_at else c.fb
        want = expected_values(c.pf, a, w, h)
        bad, unstable, first = 0, 0, None
        for y in range(h):
            row = slice(y * w, (y + 1) * w)
            if fb[row] == want[row]:
                continue
            for x in range(y * w, (y + 1) * w):
                if a[3 * x:3 * x + 3] != b[3 * x:3 * x + 3]:
                    unstable += 1
                elif fb[x] != want[x]:
                    bad += 1
                    if first is None:
                        first = (x % w, y, fb[x], want[x])
        if bad == 0 and unstable < w * h // 50:
            return unstable
        last = f"{bad} pixels differ (first at {first}), {unstable} changed between the screendumps"
        g.vm.screenshot(BUILD / f"{g.vm.name}-mismatch.png")
        time.sleep(1)
    raise AssertionError(f"{what}: {last}")


def wait_log(g, pattern, timeout=30):
    deadline = time.time() + scaled(timeout)
    while True:
        text = g.out("dmesg | tail -60")
        if re.search(pattern, text):
            return text
        assert time.time() < deadline, f"never saw /{pattern}/ in the log:\n{text}"
        time.sleep(1)


@test
def vnc_desktop_view():
    """Password auth (right, wrong, none), full updates in Raw, Hextile and ZRLE and in a 16-bit format equal to QEMU's screendump,
    an incremental update for a new window, the pointer's shape (arrow, then the terminal's I-beam); measures the throughput"""
    with VncGuest("t-vnc-view") as g:
        with_pw = g.viewer()
        assert with_pw.offered == [2], with_pw.offered                      # listening on every interface: never None
        try:
            g.viewer(password="wrongpw1")
            raise AssertionError("a wrong password got in")
        except RFBAuthFailed as e:
            assert "not accepted" in e.reason, e.reason
        try:
            g.viewer(password=None)
            raise AssertionError("no password got in")
        except RFBError:
            pass
        assert "wrong VNC password" in g.log()
        c = with_pw
        assert (c.width, c.height) == (1024, 768) and "TouhouOS" in c.name, (c.width, c.height, c.name)
        c.pointer(1000, 700)                                                # the pointer where it hides nothing interesting
        time.sleep(0.5)
        timings = {}
        for enc in ("raw", "hextile", "zrle"):
            c.set_encodings([enc])
            compare(g, c, what=enc)
            t0, n0 = time.time(), c.bytes_in
            for _ in range(3):                                              # throughput: three full updates, nothing else
                c.update(incremental=False, timeout=scaled(60))
            timings[enc] = ((c.bytes_in - n0) / 3, (time.time() - t0) / 3)
        print("     a full 1024x768 update (QEMU user networking; the time includes this script's decoding): " +
              ", ".join(f"{e} {b / 1024:.0f} KB in {t:.2f} s" for e, (b, t) in timings.items()), flush=True)
        c.set_pixel_format(PixelFormat(16, 16, False, True, 31, 63, 31, 11, 5, 0))
        c.set_encodings(["zrle"])
        compare(g, c, what="16-bit 565, ZRLE")
        c.set_pixel_format(PixelFormat())
        c.set_encodings(["copyrect", "zrle", "cursor"])
        c.update(incremental=False)
        # the pointer's shape: the arrow (26 x 26 at ui scale 1, hot spot 1,1, white with a black outline)
        deadline = time.time() + scaled(20)
        while not c.cursor or c.cursor[2] != 26:
            assert time.time() < deadline, f"no arrow shape: {c.cursor and c.cursor[:4]}"
            c.update(incremental=True, timeout=scaled(10))
        hx, hy, cw, ch, px, mask = c.cursor
        assert (hx, hy, cw, ch) == (1, 1, 26, 26), (hx, hy, cw, ch)
        colours = {c.rgb_of(px[j * cw + i]) for j in range(ch) for i in range(cw) if mask[j * 4 + i // 8] & (0x80 >> (i % 8))}
        assert colours == {(255, 255, 255), (0, 0, 0)}, colours
        compare(g, c, cursor_at=(1000, 700), what="ZRLE with the viewer drawing the pointer")
        # a new window: the incremental update covers it
        c.update(incremental=False)
        before = c.bytes_in
        g.vm.send("calculator &\n")
        deadline = time.time() + scaled(40)
        seen = []
        while True:
            c.update(incremental=True, timeout=scaled(30))
            seen += [r for r in c.rect_log if r[4] != "cursor"]
            if any(x <= 60 and y <= 50 and x + w >= 200 and y + h >= 150 for x, y, w, h, e in seen):
                break
            assert time.time() < deadline, f"no update for the new window: {seen}"
        assert c.bytes_in - before < 1024 * 768 * 4, "the whole screen was sent instead of the window"
        time.sleep(2)
        compare(g, c, cursor_at=(1000, 700), what="after the calculator opened")
        # over the calculator's buttons the pointer is a hand or an arrow; over a terminal it is the I-beam
        g.vm.send("gapterm &\n")
        time.sleep(4)
        c.pointer(300, 250)
        deadline = time.time() + scaled(30)
        while not c.cursor or c.cursor[:2] == (1, 1):
            assert time.time() < deadline, f"the shape did not change over the terminal: {c.cursor and c.cursor[:4]}"
            c.update(incremental=True, timeout=scaled(10))
            c.pointer(300 + int(time.time()) % 5, 250)
        assert c.cursor[:2] == (6, 10), c.cursor[:4]                        # the I-beam's hot spot
        g.vm.screenshot(BUILD / "t-vnc-view.png")


@test
def vnc_input():
    """Keys typed through VNC open a terminal (Ctrl+Alt+T) and run a command there (the guest's log shows its output); the viewer's
    clipboard pastes into the terminal and a word copied there reaches the viewer; dragging a window's title bar arrives as CopyRect
    and leaves the right picture; a click on the start button opens the start menu"""
    with VncGuest("t-vnc-input") as g:
        c = g.viewer()
        c.set_encodings(["copyrect", "zrle", "cursor"])
        c.update(incremental=False)
        c.pointer(600, 400)
        time.sleep(0.5)
        c.cut_text("logger -t vnctest pasted-$((7*6))-Ok")          # the clipboard, before the terminal takes the focus (and reads it)
        time.sleep(1)
        c.key(XK["Control_L"], True)
        c.key(XK["Alt_L"], True)
        c.tap(ord("t"))
        c.key(XK["Alt_L"], False)
        c.key(XK["Control_L"], False)
        time.sleep(scaled(5))
        for attempt in range(4):
            c.type_text("logger -t vnctest typed-$((6*7))-Ok\n")
            try:
                g.vm.expect(r"typed-42-Ok", timeout=15)
                break
            except TimeoutError:
                assert attempt < 3, "the command typed through VNC never ran:\n" + g.vm.text()[-1500:]
                time.sleep(3)
        # the viewer's clipboard pasted into the terminal (Ctrl+Shift+V), and a word copied in the terminal back to the viewer
        c.key(XK["Control_L"], True)
        c.key(XK["Shift_L"], True)
        c.tap(ord("V"))
        c.key(XK["Shift_L"], False)
        c.key(XK["Control_L"], False)
        c.tap(XK["Return"])
        g.vm.expect(r"pasted-42-Ok", timeout=scaled(20))
        c.type_text("clear; echo shrine77copy\n")
        time.sleep(scaled(2))
        c.click(70, 64)                                              # the terminal's first row: content origin 44,56, 8x16 cells
        time.sleep(0.05)
        c.click(70, 64)                                              # a double click selects the word and copies it
        deadline = time.time() + scaled(20)
        while "shrine77copy" not in c.cut_texts:
            assert time.time() < deadline, f"the copied word never reached the viewer: {c.cut_texts}"
            c.update(incremental=True, timeout=scaled(5))
        print(f"     clipboard: pasted from the viewer, and the viewer got {c.cut_texts[-1]!r}", flush=True)
        # the terminal scrolling (no window moves) is found by vncd and sent as CopyRect: fill the terminal, then each Return at the
        # bottom scrolls it by one line (output faster than the viewer looks, `seq 1 400` say, moves more than a window between two
        # frames, and then there is nothing to copy)
        c.type_text("seq 1 40\n")
        time.sleep(scaled(3))
        c.update(incremental=False)
        scrolled = 0
        for _ in range(10):
            c.tap(XK["Return"])
            time.sleep(0.6)
            c.update(incremental=True, timeout=scaled(10))
            scrolled += sum(1 for r in c.rect_log if r[4] == "copyrect")
            if scrolled:
                break
        assert scrolled, "the terminal scrolled line by line without a single CopyRect"
        time.sleep(scaled(2))
        compare(g, c, cursor_at=(70, 64), what="after the terminal scrolled")
        # the terminal is the first window: outer rectangle at 40,30, title bar 34..56; drag it by the title
        c.update(incremental=False)
        x0, y0 = 300, 42
        c.pointer(x0, y0, 0)
        time.sleep(0.3)
        c.pointer(x0, y0, 1)
        time.sleep(0.3)
        copies = 0
        for i in range(1, 11):
            c.pointer(x0 + 18 * i, y0 + 15 * i, 1)
            time.sleep(0.15)
            c.update(incremental=True, timeout=scaled(30))
            copies += sum(1 for r in c.rect_log if r[4] == "copyrect")
        c.pointer(x0 + 180, y0 + 150, 0)
        time.sleep(1)
        assert copies > 0, "the window moved without a single CopyRect"
        print(f"     {copies} CopyRect rectangles while the window was dragged", flush=True)
        compare(g, c, cursor_at=(x0 + 180, y0 + 150), what="after the drag")
        # the start button
        w, h, before = g.vm.screen_ppm()
        c.click(*START_BUTTON)
        deadline = time.time() + scaled(20)
        while True:
            time.sleep(0.5)
            _, _, after = g.vm.screen_ppm()
            changed = sum(1 for y in range(450, 740, 4) for x in range(4, 240, 4) if after[3 * (y * w + x):3 * (y * w + x) + 3] != before[3 * (y * w + x):3 * (y * w + x) + 3])
            if changed > 500:
                break
            assert time.time() < deadline, f"the start menu did not open ({changed} pixels changed)"
        compare(g, c, cursor_at=START_BUTTON, what="with the start menu open")
        c.tap(XK["Escape"])
        g.vm.screenshot(BUILD / "t-vnc-input.png")


@test
def vnc_resize():
    """vnc.resize: a viewer's SetDesktopSize makes the desktop 800x600 inside the 1024x768 framebuffer (the rest black, the panel at
    the new bottom); the asker gets ExtendedDesktopSize reason 1, a DesktopSize viewer the new size, and both the right picture"""
    with VncGuest("t-vnc-resize", cmdline="vnc.resize") as g:
        a = g.viewer()
        a.set_encodings(["zrle", "extdesktopsize", "cursor"])
        a.update(incremental=False)
        assert a.size_events and a.size_events[-1][:5] == (-308, 0, 0, 1024, 768), a.size_events
        b = g.viewer()
        b.set_encodings(["hextile", "desktopsize"])
        b.update(incremental=False)
        a.pointer(500, 300)
        a.set_desktop_size(800, 600)
        deadline = time.time() + scaled(30)
        while (a.width, a.height) != (800, 600):
            assert time.time() < deadline, f"no resize: {a.size_events}"
            a.update(incremental=True, timeout=scaled(20))
        assert a.size_events[-1][:5] == (-308, 1, 0, 800, 600), a.size_events
        deadline = time.time() + scaled(30)
        while (b.width, b.height) != (800, 600):
            assert time.time() < deadline, f"the other viewer did not follow: {b.size_events}"
            b.update(incremental=True, timeout=scaled(20))
        assert b.size_events[-1][:5] == (-223, 0, 0, 800, 600), b.size_events
        wait_log(g, r"the desktop is 800x600 now")
        time.sleep(3)                                                        # the shell redraws for the new size
        for v, name in ((b, "DesktopSize viewer"), (a, "ExtendedDesktopSize viewer")):
            v.update(incremental=False, timeout=scaled(60))
            w, h, shot = g.vm.screen_ppm()
            assert (w, h) == (1024, 768)
            crop = b"".join(shot[3 * (y * w):3 * (y * w + 800)] for y in range(600))
            want = expected_values(v.pf, crop, 800, 600)
            fb = v.with_cursor(500, 300) if v is a else v.fb
            bad = sum(1 for i in range(800 * 600) if fb[i] != want[i])
            assert bad < 800 * 600 // 100, f"{name}: {bad} pixels differ from the screen's top-left 800x600"
        assert all(shot[3 * (y * w + x):3 * (y * w + x) + 3] == b"\0\0\0" for y in (100, 650, 760) for x in (900, 1000)), "outside 800x600 is not black"
        assert near(b.rgb_at(118, 588), SHRINE_RED, 70), f"no start button at the new bottom: {b.rgb_at(118, 588)}"
        a.set_desktop_size(1024, 768)
        deadline = time.time() + scaled(30)
        while (a.width, a.height) != (1024, 768):
            assert time.time() < deadline, f"no resize back: {a.size_events}"
            a.update(incremental=True, timeout=scaled(20))
        g.vm.screenshot(BUILD / "t-vnc-resize.png")


@test
def vnc_robustness():
    """50 hostile streams (before and after a login) leave vncd and the desktop running; a viewer that never reads does not stall the
    compositor or the other viewer and is dropped after vnc.stall; 30 reconnects; the picture is right afterwards"""
    with VncGuest("t-vnc-robust", cmdline="vnc.stall=10") as g:
        # with a password (listen-all never offers None) the streams first meet VNC Authentication, as an attacker would ...
        streams = hostile_streams(1024, 768)
        assert len(streams) >= 50
        for data in streams:
            send_hostile("127.0.0.1", g.port, data, linger=0.2)
        # ... and the ones about the messages after the login are sent again by a viewer that logged in
        for data in streams[16:]:
            try:
                s = socket.create_connection(("127.0.0.1", g.port), timeout=20)
                v = RFBClient("127.0.0.1", g.port, password=PASSWORD, connect=False)
                v.sock = s
                v.handshake()
                s.sendall(data[len(b"RFB 003.008\n\x01\x01"):])
                s.settimeout(0.3)
                try:
                    while s.recv(65536):
                        pass
                except OSError:
                    pass
                s.close()
            except (OSError, RFBError):
                pass
        log = g.log()
        assert "bad ProtocolVersion" in log and "unknown message type" in log, log[-2000:]
        c = g.viewer()
        c.set_encodings(["zrle"])                                           # no Cursor encoding: the pointer moving is an update
        c.pointer(1000, 700)
        compare(g, c, what="after the hostile streams")
        # a viewer that asks and never reads
        stuck = socket.create_connection(("127.0.0.1", g.port), timeout=20)
        stuck.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        sv = RFBClient("127.0.0.1", g.port, password=PASSWORD, connect=False)
        sv.sock = stuck
        sv.handshake()
        stuck.sendall(struct.pack(">BxHi", 2, 1, 0))                         # Raw only: big updates
        stat0 = g.out("cat /tmp/fairyd.stat")
        t0 = time.time()
        moves = 0
        while time.time() - t0 < scaled(14):
            try:
                stuck.sendall(b"\x03\x00" + struct.pack(">HHHH", 0, 0, 1024, 768))
            except OSError:
                break
            c.pointer(300 + (moves % 40) * 10, 300)
            moves += 1
            c.update(incremental=True, timeout=scaled(20))
        stat1 = g.out("cat /tmp/fairyd.stat")
        f0 = int(re.search(r"frames (\d+)", stat0).group(1))
        f1 = int(re.search(r"frames (\d+)", stat1).group(1))
        assert f1 - f0 > 20, f"the compositor stalled: {f0} -> {f1} frames"
        log = wait_log(g, r"the viewer stopped reading", timeout=40)
        stuck.close()
        print(f"     the compositor drew {f1 - f0} frames while a viewer was stuck; the other viewer got {moves} updates", flush=True)
        # many reconnects
        for i in range(30):
            v = RFBClient("127.0.0.1", g.port, password=PASSWORD, timeout=scaled(60))
            v.update(incremental=False, area=(0, 0, 64, 64))
            v.close()
        compare(g, c, what="after 30 reconnects")
        ps = g.out("ps")
        assert "vncd" in ps and "fairyd" in ps, ps
        assert "server ended" not in g.log(), g.log()[-1500:]               # the supervisor never had to start it again


@test
def vnc_throttle_and_defaults():
    """The default listens on 127.0.0.1 only (the host cannot connect, the guest's nc sees the banner); with vnc.listen=all and the
    default throttle three wrong passwords turn the address away for a minute, even with the right password"""
    with VncGuest("t-vnc-loopback", listen_all=False, password=None, desktop=False) as g:
        log = g.log()
        assert "keeps watch on 127.0.0.1:5900" in log and "no password is set" in log, log
        try:
            c = RFBClient("127.0.0.1", g.port, timeout=10)
            c.close()
            raise AssertionError("the host reached a server that should listen on 127.0.0.1 only")
        except (OSError, RFBError):
            pass
        out = g.out("echo | nc 127.0.0.1 5900 | head -c 12; echo")
        assert "RFB 003.008" in out, out
    with VncGuest("t-vnc-throttle", cmdline="vnc.throttle=3", desktop=False) as g:
        for _ in range(3):
            try:
                g.viewer(password="nopenope")
                raise AssertionError("a wrong password got in")
            except RFBAuthFailed:
                pass
        try:
            g.viewer()
            raise AssertionError("the right password got in during the block")
        except RFBRefused as e:
            assert "too many wrong passwords" in e.reason, e.reason
        assert "Hatate looks away from that address" in g.log()


@test
def vnc_password_kept():
    """vncd -passwd writes /etc/vnc.passwd (0600 root) and a copy on the HUMANVILLAGE volume; after a restart vncd puts it back
    and the same password works"""
    from case_etc import hv_image, read_kept
    with hv_image("vnc-passwd") as img:
        with VncGuest("t-vnc-pw1", disk=img.path, desktop=False) as g:
            out = g.out("ls -l /etc/vnc.passwd")
            assert "-rw-------" in out and "root" in out, out
            g.viewer()
            g.vm.send("poweroff\n")
            assert g.vm.wait_exit(40) is not None, "no power off"
        img.check()
        assert read_kept(img, "vnc.passwd") == (PASSWORD + "\n").encode(), read_kept(img, "vnc.passwd")
        with VncGuest("t-vnc-pw2", disk=img.path, password=None, desktop=False) as g:
            assert "restored from the Human Village's records" in g.log(), g.log()
            g.viewer()
            try:
                g.viewer(password="otherpw1")
                raise AssertionError("a wrong password got in")
            except RFBAuthFailed:
                pass


@test
def vnc_desktop_of_an_ordinary_user():
    """The window server and the shell run as reimu (as under init's session) and vncd as root: vncd must take the frame file of another user's window
    server. It used to turn it away (the kernel told a client the server was root until the server had called accept()): on the Dell the viewer got an
    empty 1024x768 desktop while the screen showed the real one"""
    with VncGuest("t-vnc-reimu", desktop=False) as g:
        g.start_desktop(as_user="reimu")
        log = g.log()
        assert "is not what the window server said" not in log, log
        c = g.viewer()
        compare(g, c, what="the viewer's picture of reimu's desktop")
        assert "is not what the window server said" not in g.log(), g.log()


@test
def vnc_idle_uses_no_cpu():
    """vncd with nobody watching, and with a viewer that has asked for nothing since its last update, sleeps: on the Dell it ran at 100% of a core for the 40
    minutes since the start (2026-11-03: 9 W and 50 C at an idle desktop instead of 3.7 W and 32 C)"""
    with VncGuest("t-vnc-idle", desktop=False) as g:
        g.start_desktop(as_user="reimu")

        def cpu_ms():
            out = g.out("for p in $(ps | awk '/ vncd$/ {print $1}'); do cut -d' ' -f9 /proc/$p/stat; done")
            return sorted(int(x) for x in out.split() if x.isdigit())

        def burn(seconds=6):
            a = cpu_ms()
            time.sleep(seconds)
            b = cpu_ms()
            assert len(a) == len(b) and a, (a, b)
            return [(y - x) / (seconds * 1000.0) for x, y in zip(a, b)]

        use = burn()
        assert max(use) < 0.25, f"vncd uses {max(use):.0%} of a CPU with nobody connected (per process: {use})"
        c = g.viewer()
        c.update(incremental=False, timeout=scaled(60))
        time.sleep(2)
        use = burn()
        assert max(use) < 0.25, f"vncd uses {max(use):.0%} of a CPU with a viewer that has all it asked for (per process: {use})"
        c.close()
        time.sleep(2)
        use = burn()
        assert max(use) < 0.25, f"vncd uses {max(use):.0%} of a CPU after the viewer left (per process: {use})"
