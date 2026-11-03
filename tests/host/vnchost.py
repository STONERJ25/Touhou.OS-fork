"""Host-side helpers for the vncd tests (tests/host/test_vnc.py): vncd built for Linux with AddressSanitizer and UBSan, and a fake
window server that speaks fairyd's frame protocol (user/bin/fairyd/framepub.h) over a UNIX socket, with a picture the tests draw
into. Nothing here needs QEMU; tests/case_vnc.py does the same against the real desktop.
"""
import mmap
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tests"))
OUT = REPO / "build" / "host-vnc"

FM_HELLO, FM_CLIPBOARD_SET, FM_CLIPBOARD_GET, FM_SET_MODE = 1, 14, 15, 24
FM_FRAME_SUBSCRIBE, FM_FRAME_REQUEST = 64, 65
FE_WELCOME, FE_CLIPBOARD, FE_ERROR = 100, 108, 110
FE_FRAME_READY, FE_FRAME = 164, 165
PIXELS_OFF, CURSOR_MAX = 4096, 128
FRAME_F_FULL, FRAME_F_RESIZED, FRAME_F_CURSOR = 1, 2, 4


def build():
    """vncd for Linux, once per test run (rebuilt when a source is newer)."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "vncd"
    srcs = sorted((REPO / "user" / "bin" / "vncd").glob("*.c")) + [REPO / "user/lib/z/src/crc.c", REPO / "user/lib/z/src/deflate.c"]
    heads = list((REPO / "user" / "bin" / "vncd").glob("*.h")) + [REPO / "user/bin/fairyd/framepub.h", REPO / "user/lib/fairy/include/fairy.h"]
    if exe.exists() and all(exe.stat().st_mtime > p.stat().st_mtime for p in srcs + heads):
        return exe
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer",
           "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers", "-D_GNU_SOURCE",
           "-I", str(REPO / "user/bin/vncd"), "-I", str(REPO / "user/lib/fairy/include"), "-I", str(REPO / "user/lib/z/include"),
           "-idirafter", str(REPO / "shared"), *map(str, srcs), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("building vncd for the host failed:\n" + r.stdout + r.stderr)
    return exe


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeFairy:
    """A window server with nothing but a picture. Changes made with fill()/move()/... are published the way fairyd does it: copied
    into the shared memory only when vncd has a request outstanding, then announced with FE_FRAME."""

    def __init__(self, workdir, w=320, h=240, max_w=None, max_h=None, scale=1, background=0x0A0F2A):
        self.dir = Path(workdir)
        self.sock_path = str(self.dir / "fairyd.sock")
        self.w, self.h = w, h
        self.max_w, self.max_h = max_w or w, max_h or h
        self.scale = scale
        self.screen = bytearray(self.max_w * self.max_h * 4)
        self.lock = threading.RLock()
        self.conn = None
        self.inbuf = b""
        self.shm = None
        self.pending = False
        self.full = True
        self.resized = False
        self.damage = []
        self.moves = []
        self.ptr = (w // 2, h // 2)
        self.cursor = None                      # (w, h, hotx, hoty, [argb...])
        self.cursor_serial = 1
        self.cursor_new = True
        self.mods, self.numlock = 0, 1
        self.clip, self.clip_serial = b"", 0
        self.mode_requests, self.clip_sets = [], []
        self.subscribed = threading.Event()
        self.requests = 0
        self.frames = 0
        self.seq = 0
        self.refuse = False
        self.stopped = False
        self.events_log = []
        self.fill(0, 0, w, h, background, damage=False)
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        self.ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.ls.bind(self.sock_path)
        self.ls.listen(4)
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    # ------------------------------------------------------------------------------------------------ the picture
    def pixel(self, x, y):
        return struct.unpack_from("<I", self.screen, (y * self.max_w + x) * 4)[0]

    def fill(self, x, y, w, h, rgb, damage=True):
        row = struct.pack("<I", rgb) * w
        with self.lock:
            for j in range(y, y + h):
                o = (j * self.max_w + x) * 4
                self.screen[o:o + 4 * w] = row
            if damage:
                self.damage.append((x, y, w, h))
                self._maybe_send()

    def put(self, x, y, w, h, values, damage=True):
        """values: w*h 0x00RRGGBB."""
        with self.lock:
            for j in range(h):
                o = ((y + j) * self.max_w + x) * 4
                self.screen[o:o + 4 * w] = struct.pack(f"<{w}I", *values[j * w:(j + 1) * w])
            if damage:
                self.damage.append((x, y, w, h))
                self._maybe_send()

    def move(self, x, y, w, h, dx, dy, exposed=0x0A0F2A, hint=True):
        """A "window" at x,y,w,h moves by dx,dy: its pixels go along, the uncovered part becomes `exposed`."""
        with self.lock:
            rows = [bytes(self.screen[((y + j) * self.max_w + x) * 4:((y + j) * self.max_w + x + w) * 4]) for j in range(h)]
            self.fill(x, y, w, h, exposed, damage=False)
            for j in range(h):
                o = ((y + dy + j) * self.max_w + x + dx) * 4
                self.screen[o:o + 4 * w] = rows[j]
            self.damage.append((x, y, w, h))
            self.damage.append((x + dx, y + dy, w, h))
            if hint:                                            # like fairyd: one hint per window, from where it was in the last frame
                for i, (mx, my, mw, mh, mdx, mdy) in enumerate(self.moves):
                    if (mx, my, mw, mh) == (x, y, w, h):
                        self.moves[i] = (x + dx, y + dy, w, h, mdx + dx, mdy + dy)
                        break
                else:
                    self.moves.append((x + dx, y + dy, w, h, dx, dy))
            self._maybe_send()

    def scroll(self, x, y, w, h, lines, new_rows):
        """The content of x,y,w,h moves up by `lines` rows (a terminal printing); `new_rows` (lines*w values) fill the bottom. Damage only,
        no hint: vncd has to find the scroll itself."""
        with self.lock:
            rows = [bytes(self.screen[((y + j) * self.max_w + x) * 4:((y + j) * self.max_w + x + w) * 4]) for j in range(h)]
            for j in range(h - lines):
                o = ((y + j) * self.max_w + x) * 4
                self.screen[o:o + 4 * w] = rows[j + lines]
            for j in range(lines):
                o = ((y + h - lines + j) * self.max_w + x) * 4
                self.screen[o:o + 4 * w] = struct.pack(f"<{w}I", *new_rows[j * w:(j + 1) * w])
            self.damage.append((x, y, w, h))
            self._maybe_send()

    def set_pointer(self, x, y):
        with self.lock:
            self.ptr = (x, y)
            self._maybe_send(force=True)

    def set_cursor(self, w, h, hotx, hoty, argb):
        with self.lock:
            self.cursor = (w, h, hotx, hoty, list(argb))
            self.cursor_serial += 1
            self.cursor_new = True
            self._maybe_send(force=True)

    def set_mode(self, w, h):
        with self.lock:
            self.w, self.h = w, h
            self.resized = True
            self._maybe_send()

    def set_clipboard(self, text):
        with self.lock:
            self.clip = text.encode("utf-8")
            self.clip_serial += 1
            self._maybe_send(force=True)

    def rgb_bytes(self):
        """The screen in use as packed RGB."""
        out = bytearray()
        for y in range(self.h):
            row = struct.unpack_from(f"<{self.w}I", self.screen, y * self.max_w * 4)
            for v in row:
                out += bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255))
        return bytes(out)

    def values(self):
        out = []
        for y in range(self.h):
            out += list(struct.unpack_from(f"<{self.w}I", self.screen, y * self.max_w * 4))
        return out

    # ------------------------------------------------------------------------------------------------ the protocol
    def _send(self, mtype, payload=b""):
        if not self.conn:
            return
        try:
            self.conn.sendall(struct.pack("<IHHII", 16 + len(payload), mtype, 0, 0, 0) + payload)
        except OSError:
            self._drop()

    def _drop(self):
        if self.conn:
            try:
                self.conn.close()
            except OSError:
                pass
        self.conn = None
        self.pending = False

    def disconnect(self):
        """Like a window server that ends: the next connection starts over."""
        with self.lock:
            self._drop()
            self.subscribed.clear()
            self.full = True

    def _maybe_send(self, force=False):
        self._force = getattr(self, "_force", False) or force
        if not (self.pending and self.shm and self.conn):
            return
        if not (self.full or self.resized or self.damage or self.moves or self._force or self.cursor_new):
            return
        flags = 0
        view = memoryview(self.shm)
        if self.full or self.resized:
            for y in range(self.h):
                o = y * self.max_w * 4
                view[PIXELS_OFF + o:PIXELS_OFF + o + self.w * 4] = self.screen[o:o + self.w * 4]
            flags |= FRAME_F_FULL | (FRAME_F_RESIZED if self.resized else 0)
            rects = []
        else:
            rects = []
            for (x, y, w, h) in self.damage:
                x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
                if x1 <= x0 or y1 <= y0:
                    continue
                for j in range(y0, y1):
                    o = (j * self.max_w + x0) * 4
                    view[PIXELS_OFF + o:PIXELS_OFF + o + (x1 - x0) * 4] = self.screen[o:o + (x1 - x0) * 4]
                rects.append((x0, y0, x1 - x0, y1 - y0))
        if len(rects) > 32:                                     # like fairyd: the bounding box when the list is full
            xs = [r[0] for r in rects] + [r[0] + r[2] for r in rects]
            ys = [r[1] for r in rects] + [r[1] + r[3] for r in rects]
            rects = [(min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))]
        moves = [] if (self.full or self.resized) else self.moves[:4]
        cw = ch = hx = hy = 0
        if self.cursor:
            cw, ch, hx, hy, argb = self.cursor
            if self.cursor_new:
                base = PIXELS_OFF + self.max_w * self.max_h * 4
                for j in range(ch):
                    view[base + j * CURSOR_MAX * 4:base + j * CURSOR_MAX * 4 + cw * 4] = struct.pack(f"<{cw}I", *argb[j * cw:(j + 1) * cw])
                flags |= FRAME_F_CURSOR
        self.seq += 1
        msg = struct.pack("<IIiiiiiiiiiIBBHIHH", self.seq, flags, self.w, self.h, self.scale, self.ptr[0], self.ptr[1], cw, ch, hx, hy,
                          self.cursor_serial, self.mods, self.numlock, 0, self.clip_serial, len(rects), len(moves))
        msg += b"".join(struct.pack("<iiii", *r) for r in rects) + b"".join(struct.pack("<iiiiii", *m) for m in moves)
        self.full = self.resized = self.cursor_new = self._force = False
        self.damage, self.moves = [], []
        self.pending = False
        self.frames += 1
        self._send(FE_FRAME, msg)

    def _subscribe(self):
        if self.refuse:
            self._send(FE_ERROR, struct.pack("<i64s", 4, b"the frame service is for root only"))
            return
        path = self.dir / f"frame-{os.getpid()}-{self.frames}.buf"
        size = PIXELS_OFF + self.max_w * self.max_h * 4 + CURSOR_MAX * CURSOR_MAX * 4
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
        os.ftruncate(fd, size)
        self.shm = mmap.mmap(fd, size)
        os.close(fd)
        self.shm[0:32] = struct.pack("<IIIIiiII", 0x504D5246, 1, PIXELS_OFF, self.max_w, self.max_w, self.max_h,
                                     PIXELS_OFF + self.max_w * self.max_h * 4, CURSOR_MAX)
        self.shm_path = str(path)
        self.full = True
        self.cursor_new = True
        self._send(FE_FRAME_READY, struct.pack("<I64sIii", 1, str(path).encode(), size, self.max_w, self.max_h))
        self.subscribed.set()

    def _handle(self, mtype, payload):
        self.events_log.append(mtype)
        if mtype == FM_HELLO:
            self._send(FE_WELCOME, struct.pack("<IIiiiiiii", 1, 1, self.w // self.scale, self.h // self.scale, 0, 0, self.w, self.h, self.scale))
        elif mtype == FM_FRAME_SUBSCRIBE:
            self._subscribe()
        elif mtype == FM_FRAME_REQUEST:
            self.requests += 1
            (flags,) = struct.unpack_from("<I", payload)
            if flags & 1:
                self.full = True
            self.pending = True
            self._maybe_send(force=bool(flags & 2))          # FRAMEREQ_NOW: answer even if nothing changed
        elif mtype == FM_CLIPBOARD_GET:
            self._send(FE_CLIPBOARD, struct.pack("<I", len(self.clip)) + self.clip)
        elif mtype == FM_CLIPBOARD_SET:
            (n,) = struct.unpack_from("<I", payload)
            self.clip_sets.append(payload[4:4 + n].decode("utf-8"))
        elif mtype == FM_SET_MODE:
            self.mode_requests.append(struct.unpack_from("<ii", payload))

    def _loop(self):
        while not self.stopped:
            fds = [self.ls] + ([self.conn] if self.conn else [])
            try:
                r, _, _ = select.select(fds, [], [], 0.05)
            except (OSError, ValueError):
                continue
            with self.lock:
                if self.ls in r and not self.conn:
                    self.conn, _ = self.ls.accept()
                    self.inbuf = b""
                if self.conn and self.conn in r:
                    try:
                        data = self.conn.recv(65536)
                    except OSError:
                        data = b""
                    if not data:
                        self._drop()
                        continue
                    self.inbuf += data
                    while len(self.inbuf) >= 16:
                        size, mtype = struct.unpack_from("<IH", self.inbuf)
                        if len(self.inbuf) < size:
                            break
                        payload, self.inbuf = self.inbuf[16:size], self.inbuf[size:]
                        self._handle(mtype, payload)

    def close(self):
        self.stopped = True
        self._drop()
        self.ls.close()


class Vncd:
    """vncd (the host build) with a fake window server; .port is where viewers connect."""

    def __init__(self, fairy, password=None, args=(), cmdline="", listen_all=False, keep_file=None, limits=True):
        self.exe = build()
        self.fairy = fairy
        self.port = free_port()
        self.dir = fairy.dir
        self.vinput = self.dir / "vinput.bin"
        self.pwfile = self.dir / "vnc.passwd"
        self.log = self.dir / "vncd.log"
        if self.vinput.exists():
            self.vinput.unlink()
        if password is not None:
            self.set_password(password)
        elif self.pwfile.exists():
            self.pwfile.unlink()
        argv = [str(self.exe), "--port", str(self.port), "--fairy", fairy.sock_path, "--vinput", str(self.vinput), "--passwd-file", str(self.pwfile),
                "--keep-file", str(keep_file) if keep_file else "none", "--fail-delay", "50", "--handshake", "3000", "--stall", "4000",
                *(["--throttle", "0", "--peraddr", "0", "--maxclients", "16"] if limits else []), *(["--listen-all"] if listen_all else []), *args]
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1", UBSAN_OPTIONS="print_stacktrace=1", VNCD_CMDLINE=cmdline)
        self.logf = open(self.log, "wb")
        self.proc = subprocess.Popen(argv, stdout=self.logf, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + 20
        while "keeps watch" not in self.text() and "WARNING: listening" not in self.text():
            if self.proc.poll() is not None:
                raise RuntimeError("vncd ended at start:\n" + self.text())
            if time.time() > deadline:
                raise RuntimeError("vncd never started listening:\n" + self.text())
            time.sleep(0.05)

    def set_password(self, pw):
        self.pwfile.write_text(pw + "\n")
        os.chmod(self.pwfile, 0o600)

    def text(self):
        try:
            return self.log.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def alive(self):
        return self.proc.poll() is None

    def events(self):
        """The records written to the virtual input device: (type, code, value)."""
        try:
            data = self.vinput.read_bytes()
        except FileNotFoundError:
            return []
        return [struct.unpack_from("<BxHi", data, i)[:3] for i in range(0, len(data) - len(data) % 16, 16)]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.logf.close()
        text = self.text()
        if "AddressSanitizer" in text or "runtime error" in text or "LeakSanitizer" in text:
            raise AssertionError("vncd: a sanitizer complained:\n" + text[-4000:])
        return self.proc.returncode


def workdir():
    return tempfile.mkdtemp(prefix="vnct-")
