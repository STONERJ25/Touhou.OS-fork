"""A tiny terminal emulator for tests: replays the raw bytes the guest wrote to the serial console and keeps the screen.

It understands what the kernel framebuffer console understands (printable UTF-8, CR/LF/BS/TAB, CSI cursor movement,
erase, SGR colours, save/restore cursor) and ignores the rest, including private modes such as the alternate screen.
Wrapping works like the console: a glyph written in the last column waits, and the next glyph moves to the next row.

    screen = VT(48, 128)
    screen.feed(vm.serial_path.read_bytes())
    screen.rows_text()      # list of strings, trailing blanks removed
"""
import codecs
import re

_CSI = re.compile(rb"\x1b\[([0-9;?]*)([@-~])")


class VT:
    def __init__(self, rows=48, cols=128):
        self.rows, self.cols = rows, cols
        self.cells = [[(" ", 0)] * cols for _ in range(rows)]
        self.x = self.y = 0
        self.saved = (0, 0)
        self.attr = 0                   # bit0 bold, bit1 reverse, bits 8-15 fg+1, bits 16-23 bg+1
        self.bold = False
        self._dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self._buf = b""

    # ------------------------------------------------------------ output
    def _newline(self):
        self.x = 0
        self.y += 1
        if self.y >= self.rows:
            self.y = self.rows - 1
            del self.cells[0]
            self.cells.append([(" ", 0)] * self.cols)

    def _put(self, ch):
        if self.x >= self.cols:
            self._newline()
        self.cells[self.y][self.x] = (ch, self.attr)
        self.x += 1

    def _clear(self, y, x0, x1):
        for x in range(x0, min(x1, self.cols)):
            self.cells[y][x] = (" ", 0)

    def _sgr(self, params):
        if not params:
            params = [0]
        i = 0
        while i < len(params):
            p = params[i]
            if p == 0:
                self.attr = 0
                self.bold = False
            elif p == 1:
                self.bold = True
                self.attr |= 1
            elif p == 22:
                self.bold = False
                self.attr &= ~1
            elif p == 7:
                self.attr |= 2
            elif p == 27:
                self.attr &= ~2
            elif 30 <= p <= 37:
                self.attr = (self.attr & ~0xFF00) | ((p - 30 + (8 if self.bold else 0) + 1) << 8)
            elif 90 <= p <= 97:
                self.attr = (self.attr & ~0xFF00) | ((p - 90 + 8 + 1) << 8)
            elif p == 39:
                self.attr &= ~0xFF00
            elif 40 <= p <= 47:
                self.attr = (self.attr & ~0xFF0000) | ((p - 40 + 1) << 16)
            elif 100 <= p <= 107:
                self.attr = (self.attr & ~0xFF0000) | ((p - 100 + 8 + 1) << 16)
            elif p == 49:
                self.attr &= ~0xFF0000
            elif p in (38, 48) and i + 1 < len(params):
                i += 4 if params[i + 1] == 2 else 2
            i += 1

    def _csi(self, params_raw, final):
        if params_raw.startswith(b"?"):
            return
        params = [int(p) if p else 0 for p in params_raw.decode().split(";")] if params_raw else []

        def arg(i, d):
            return params[i] if i < len(params) and params[i] > 0 else d

        f = chr(final)
        if f == "m":
            self._sgr(params)
        elif f == "A":
            self.y = max(0, self.y - arg(0, 1))
        elif f == "B":
            self.y = min(self.rows - 1, self.y + arg(0, 1))
        elif f == "C":
            self.x = min(self.cols - 1, self.x + arg(0, 1))
        elif f == "D":
            self.x = max(0, self.x - arg(0, 1))
        elif f == "G":
            self.x = max(0, min(self.cols - 1, arg(0, 1) - 1))
        elif f in "Hf":
            self.y = max(0, min(self.rows - 1, arg(0, 1) - 1))
            self.x = max(0, min(self.cols - 1, arg(1, 1) - 1))
        elif f == "J":
            mode = params[0] if params else 0
            if mode in (2, 3):
                for y in range(self.rows):
                    self._clear(y, 0, self.cols)
                if mode == 2:
                    self.x = self.y = 0
            elif mode == 0:
                self._clear(self.y, self.x, self.cols)
                for y in range(self.y + 1, self.rows):
                    self._clear(y, 0, self.cols)
            elif mode == 1:
                for y in range(self.y):
                    self._clear(y, 0, self.cols)
                self._clear(self.y, 0, self.x + 1)
        elif f == "K":
            mode = params[0] if params else 0
            if mode == 0:
                self._clear(self.y, self.x, self.cols)
            elif mode == 1:
                self._clear(self.y, 0, self.x + 1)
            else:
                self._clear(self.y, 0, self.cols)
        elif f == "s":
            self.saved = (self.x, self.y)
        elif f == "u":
            self.x, self.y = self.saved

    def feed(self, data):
        data = self._buf + data
        self._buf = b""
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if b == 0x1B:
                if i + 1 >= n:
                    self._buf = data[i:]
                    break
                if data[i + 1] == ord("["):
                    m = _CSI.match(data, i)
                    if not m:
                        if n - i < 24:                      # incomplete sequence at the end of the chunk
                            self._buf = data[i:]
                            break
                        i += 2
                        continue
                    self._csi(m.group(1), m.group(2)[0])
                    i = m.end()
                    continue
                i += 2                                       # other escapes are ignored with their one argument byte
                continue
            if b == 0x0D:
                self.x = 0
            elif b == 0x0A:
                self._newline()
            elif b == 0x08:
                self.x = max(0, self.x - 1)
            elif b == 0x09:
                nxt = (self.x + 8) & ~7
                while self.x < nxt and self.x < self.cols:
                    self._put(" ")
            elif b == 0x07 or b < 0x20:
                pass
            else:
                end = i + 1
                while end < n and data[end] >= 0x20 and data[end] != 0x7F and data[end] != 0x1B:
                    end += 1
                for ch in self._dec.decode(data[i:end]):
                    self._put(ch)
                i = end
                continue
            i += 1

    # ------------------------------------------------------------ inspection
    def rows_text(self):
        return ["".join(c for c, _ in row).rstrip() for row in self.cells]

    def text(self):
        rows = self.rows_text()
        while rows and not rows[-1]:
            rows.pop()
        return "\n".join(rows)

    def row_attrs(self, y):
        return [a for _, a in self.cells[y]]

    def find(self, needle):
        """Row index of the first row containing needle, or -1."""
        for y, row in enumerate(self.rows_text()):
            if needle in row:
                return y
        return -1
