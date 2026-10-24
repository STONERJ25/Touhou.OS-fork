"""Real-hardware readiness (docs/HARDWARE_AUDIT.md): the things a real PC does that QEMU's defaults hide, reproduced with QEMU
options where that is possible: no PIT, no PAT, no PS/2 controller, other processors, memory above 4 GiB, a cmdline.txt written by
a Windows editor, and a text console whose screen is checked pixel by pixel."""
import re
import shutil
import time
from pathlib import Path

from harness import test, Qemu, BUILD

ROOT = BUILD.parent
PROMPT = r"reimu@gensokyo \S+ \$ "


# ---------------------------------------------------------------------------------------------- helpers

class RawCmdlineQemu(Qemu):
    """A Qemu whose cmdline.txt holds exactly these bytes (None: no such file on the boot volume)."""

    def __init__(self, raw, **kw):
        super().__init__(**kw)
        self._raw = raw

    def _prepare(self):
        v = super()._prepare()
        f = self.esp / "cmdline.txt"
        if self._raw is None:
            f.unlink()
        else:
            f.write_bytes(self._raw)
        return v


_GLYPHS = None


def glyphs():
    """The console font (shared/font8x16.c): code point -> 16 scanline bytes."""
    global _GLYPHS
    if _GLYPHS is None:
        src = (ROOT / "shared" / "font8x16.c").read_text(encoding="utf-8")
        _GLYPHS = {}
        for m in re.finditer(r"\{ 0x([0-9A-Fa-f]+), \{([^}]*)\} \}", src):
            _GLYPHS[int(m.group(1), 16)] = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
    return _GLYPHS


def row_pixels(text, fg, bg):
    """What the console draws for one text row: 16 scanlines of len(text) * 8 RGB pixels."""
    g = glyphs()
    lines = []
    for y in range(16):
        line = []
        for ch in text:
            bits = g[ord(ch)][y]
            for x in range(8):
                line.append(fg if bits & (0x80 >> x) else bg)
        lines.append(line)
    return lines


def screen_row_matches(shot, row, text, fg, bg):
    """True if text row `row` of the screen shows exactly `text` (and blank to the right of it) in the given colours."""
    w, h, rgb = shot
    cells = w // 8
    padded = text.ljust(cells)
    want = row_pixels(padded, fg, bg)
    for y in range(16):
        base = ((row * 16 + y) * w) * 3
        got = rgb[base:base + w * 3]
        exp = bytes(c for px in want[y] for c in px)
        if got[:len(exp)] != exp[:len(got)]:
            return False
    return True


CONSOLE_FG = (0xD3, 0xD7, 0xCF)
CONSOLE_BG = (0x0B, 0x0B, 0x14)


# ---------------------------------------------------------------------------------------------- timers and interrupts

@test
def hwaudit_pit_missing():
    """No 8254 PIT (QEMU pit=off): the TSC is calibrated from the ACPI timer, the local APIC timer becomes the tick, and tasks sleep and wake."""
    with Qemu(cmdline="text", name="t-hw-nopit", extra=["-machine", "q35,pit=off"]) as vm:
        vm.expect(r"TSC ~\d+ MHz \(ACPI timer\)", timeout=60)
        vm.expect(r"no timer interrupt arrived in 200 ms", timeout=30)
        vm.expect(r"the local APIC timer is the system tick now", timeout=30)
        vm.expect(PROMPT, timeout=120)
        vm.send("sleep 1; echo woke-$((6*7))\n")
        vm.expect(r"woke-42", timeout=60)


@test
def hwaudit_other_processors_parked():
    """With four processors the three others are sent to wait-for-SIPI (INIT) before the firmware memory they may be running in is reused."""
    with Qemu(cmdline="text", name="t-hw-smp", extra=["-smp", "4"]) as vm:
        vm.expect(r"acpi: .* 4 processors", timeout=60)
        vm.expect(r"3 other processors sent to wait-for-SIPI", timeout=30)
        vm.expect(PROMPT, timeout=120)
        vm.send("echo smp-$((6*7))\n")
        vm.expect(r"smp-42", timeout=30)


# ---------------------------------------------------------------------------------------------- CPU features

@test
def hwaudit_cpu_without_pat():
    """A CPU that does not advertise PAT: no wrmsr to a missing MSR, the framebuffer is mapped uncached and the console still works."""
    with Qemu(cmdline="text", name="t-hw-nopat", extra=["-cpu", "qemu64,-pat"]) as vm:
        vm.expect(r"\(no PAT: framebuffer uncached\)", timeout=60)
        vm.expect(r"framebuffer .* uncached \(this CPU has no PAT\)", timeout=30)
        vm.expect(PROMPT, timeout=120)
        vm.send("echo nopat-$((6*7))\n")
        vm.expect(r"nopat-42", timeout=30)


@test
def hwaudit_framebuffer_cache_knob():
    """fb.cache=uc maps the framebuffer uncached (for comparing speeds on real hardware); the speed line is always printed."""
    with Qemu(cmdline="text fb.cache=uc", name="t-hw-fbuc") as vm:
        vm.expect(r"framebuffer .* uncached \(fb\.cache=uc\)", timeout=60)
        vm.expect(r"framebuffer speed: a full-screen fill \(\d+ KiB\) took [\d.]+ ms, ~\d+ MB/s", timeout=30)
        vm.expect(PROMPT, timeout=120)


@test
def hwaudit_mtrr_selftest():
    """The write-combining MTRR fallback in the kernel self test: the decision function over made-up firmware layouts (a UC range over the
    framebuffer, no free register, an overlapping WB range, ...), the SDM update sequence on the registers QEMU emulates, and the whole
    attempt with scripted speeds (kept, undone for lack of gain, forced, a loss), with the framebuffer's page flags checked each time."""
    with Qemu(cmdline="selftest", name="t-hw-mtrr-self") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=90)
        text = vm.text()
        assert "[selftest] mtrr: ok" in text, text[-1500:]
        assert "the hardware part is skipped" not in text, text[-1500:]
        assert "SELFTEST PASS" in text, text[-1500:]


@test
def hwaudit_mtrr_forced_attempt():
    """fb.mtrr=on: the log shows the firmware's MTRR layout, the display BAR that holds the framebuffer (found by sizing it) and what came of
    the attempt: a write-combining range with both speeds, or the reason none was added (OVMF puts a UC range over its aperture)."""
    with Qemu(cmdline="text fb.mtrr=on", name="t-hw-mtrr-on") as vm:
        vm.expect(r"mtrr: (on|OFF), default (\w+), (\d+) of (\d+) variable ranges in use", timeout=90)
        m = vm.expect(r"framebuffer (0x[0-9a-f]+) lies in a prefetchable BAR of (0x[0-9a-f]+) \+ (0x[0-9a-f]+) bytes", timeout=30)
        fb, base, size = (int(m.group(i), 16) for i in (1, 2, 3))
        assert size & (size - 1) == 0 and base % size == 0, f"a BAR is a naturally aligned power of two: {base:#x} + {size:#x}"
        assert base <= fb < base + size, f"the framebuffer {fb:#x} is not inside the BAR {base:#x} + {size:#x}"
        m = vm.expect(r"framebuffer: (?:write-combining MTRR #\d+ over (0x[0-9a-f]+) \+ \d+ [MG]iB .*fb speed before (\d+) MB/s after (\d+) MB/s, "
                      r"(?:kept|no gain)|no MTRR added \(.*\); fb speed \d+ MB/s stays)", timeout=60)
        assert m.group(1) is None or int(m.group(1), 16) == base, f"the MTRR covers {m.group(1)}, the BAR starts at {base:#x}"
        vm.expect(PROMPT, timeout=120)
        vm.send("echo mtrr-$((6*7))\n")
        vm.expect(r"mtrr-42", timeout=30)
    # A CPU that does not have MTRRs at all: nothing to read, nothing to write, the framebuffer stays as the page tables map it.
    with Qemu(cmdline="text fb.mtrr=on", name="t-hw-mtrr-none", extra=["-cpu", "qemu64,-mtrr"]) as vm:
        vm.expect(r"the firmware's MTRR for it: no MTRRs", timeout=90)
        vm.expect(r"framebuffer: no MTRR added \(this CPU has no MTRRs\)", timeout=60)
        vm.expect(PROMPT, timeout=120)


@test
def hwaudit_mtrr_knob_off_and_auto():
    """fb.mtrr=off leaves the MTRRs alone and says so; with the default (auto) a fast screen leaves them alone as well."""
    with Qemu(cmdline="text fb.mtrr=off", name="t-hw-mtrr-off") as vm:
        vm.expect(r"fb\.mtrr=off: no write-combining MTRR is tried \(the framebuffer is write-combining \(PAT\)\)", timeout=90)
        vm.expect(PROMPT, timeout=120)
    with Qemu(cmdline="text", name="t-hw-mtrr-auto") as vm:
        # A host under heavy load can make the emulated screen look slow for a moment; then the attempt is made and logged instead.
        vm.expect(r"fb\.mtrr=auto: the screen takes \d+ MB/s, fast enough|framebuffer: (no MTRR added|write-combining MTRR)", timeout=90)
        vm.expect(PROMPT, timeout=120)


# ---------------------------------------------------------------------------------------------- buses and memory

@test
def hwaudit_pci_selftest():
    """PCI in the kernel self test: BAR sizes from read-backs whose upper bits read zero (a 64-bit BAR of a device with a 48-bit address width used
    to come out as exabytes), and a function put into D3hot the way firmware leaves unused ones (an e1000e, which has a power management
    capability) is woken, waits the 10 ms of the specification and keeps its BARs and command register."""
    with Qemu(cmdline="selftest", name="t-hw-pci-self", net="e1000e") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=90)
        text = vm.text()
        assert "[selftest] pci: ok" in text, text[-1500:]
        assert re.search(r"\[selftest\] pci: 00:[0-9a-f]{2}\.0 8086:[0-9a-f]{4} woke from D3hot in \d+ ms", text), text[-1500:]
        assert "SELFTEST PASS" in text, text[-1500:]


@test
def hwaudit_no_ps2_controller():
    """A USB-only PC without an i8042 (QEMU i8042=off): the floating bus is recognised, boot goes on, input comes from USB or serial."""
    with Qemu(cmdline="text", name="t-hw-nops2", extra=["-machine", "q35,i8042=off"]) as vm:
        vm.expect(r"no PS/2 controller found", timeout=60)
        vm.expect(PROMPT, timeout=120)
        vm.send("echo nops2-$((6*7))\n")
        vm.expect(r"nops2-42", timeout=30)


@test
def hwaudit_memory_above_4g():
    """6 GiB of RAM (2 GiB below the PCI hole, the rest above 4 GiB): the loader maps it, the kernel uses it, the map summary says so."""
    with Qemu(cmdline="text", name="t-hw-6g", mem="6G") as vm:
        vm.expect(r"memory map: \d+ regions, \d+ MiB usable .* RAM reaches 0x[0-9a-f]+, (\d+) MiB of it above 4 GiB", timeout=90)
        high = int(re.search(r"(\d+) MiB of it above 4 GiB", vm.text()).group(1))
        assert high >= 3000, f"expected about 4096 MiB above 4 GiB, the log says {high}"
        vm.expect(PROMPT, timeout=120)
        vm.send("grep MemTotal /proc/meminfo\n")
        m = vm.expect(r"MemTotal:\s+(\d+) kB", timeout=30)
        assert int(m.group(1)) > 5_500_000, m.group(0)


# ---------------------------------------------------------------------------------------------- the loader and cmdline.txt

CMDLINE_CASES = {
    "utf8-bom-crlf": (b"\xef\xbb\xbftext blk.ro\r\n", "text blk.ro"),
    "utf16-bom": ("text blk.ro tz=EST5EDT\r\n".encode("utf-16"), "text blk.ro tz=EST5EDT"),
    "multi-line": (b"text\n\nblk.ro\r\n  nosounds \n", "text blk.ro nosounds"),
    "tabs": (b"\ttext\tblk.ro\t\n", "text blk.ro"),
}


@test
def hwaudit_cmdline_encodings():
    """cmdline.txt saved by a Windows editor (UTF-8 BOM, UTF-16, CRLF, several lines, tabs) reaches the kernel as plain words: blk.ro must not get lost."""
    for name, (raw, expected) in CMDLINE_CASES.items():
        esp = BUILD / f"hwaudit-esp-{name}"
        shutil.copytree(BUILD / "esp", esp, dirs_exist_ok=True)
        with RawCmdlineQemu(raw, cmdline="", name=f"t-hw-cmd-{name}", esp=esp) as vm:
            m = vm.expect(r"loader: cmdline '([^']*)'", timeout=60)
            assert m.group(1) == expected, f"{name}: the loader passed '{m.group(1)}', expected '{expected}'"
            vm.expect(r"Gensokyo\] image .*, cmdline '([^']*)'", timeout=60)
            assert re.search(r"Gensokyo\] image .*, cmdline '%s'" % re.escape(expected), vm.text()), f"{name}: the kernel saw something else"


@test
def hwaudit_missing_cmdline_means_read_only():
    """No cmdline.txt on the stick at all: the loader falls back to 'video=native blk.ro' so that no disk is ever writable by accident."""
    esp = BUILD / "hwaudit-esp-missing"
    shutil.copytree(BUILD / "esp", esp, dirs_exist_ok=True)
    with RawCmdlineQemu(None, cmdline="", name="t-hw-cmd-missing", esp=esp) as vm:
        vm.expect(r"loader: no cmdline\.txt on the boot volume: using the safe default", timeout=60)
        vm.expect(r"loader: cmdline 'video=native blk\.ro'", timeout=30)
        vm.expect(r"Gensokyo\] image .*, cmdline 'video=native blk\.ro'", timeout=60)


# ---------------------------------------------------------------------------------------------- the screen

@test
def hwaudit_fbcon_selftest():
    """The text console's diff renderer: after scrolls, cursor addressing, a disabled period, one huge write and a forced redraw the screen equals the grid."""
    with Qemu(cmdline="selftest", name="t-hw-fbcon") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=90)
        text = vm.text()
        assert "[selftest] fbcon: ok" in text, text[-1500:]
        assert "SELFTEST PASS" in text, text[-1500:]


@test
def hwaudit_console_scroll_pixels():
    """500 numbered lines scroll the text console many screens; the last rows on screen are pixel-for-pixel the numbers 491..500."""
    with Qemu(cmdline="text", name="t-hw-scroll") as vm:
        vm.expect(PROMPT, timeout=120)
        vm.send("i=1; while [ $i -le 500 ]; do echo $i; i=$((i+1)); done\n")
        vm.expect(r"\n500\n", timeout=90)
        vm.expect(PROMPT, timeout=60)
        deadline = time.time() + 20
        bad = None
        while time.time() < deadline:
            shot = vm.screen_ppm()
            rows = shot[1] // 16
            bad = [500 - k for k in range(10) if not screen_row_matches(shot, rows - 2 - k, str(500 - k), CONSOLE_FG, CONSOLE_BG)]
            if not bad:
                break
            time.sleep(0.5)
        assert not bad, f"rows showing {bad} do not match the font rendering of those numbers"
        vm.screenshot(BUILD / "t-hw-scroll.png")


# ---------------------------------------------------------------------------------------------- the window server

def _desktop_stat(name, cmdline):
    from guitools import Gui
    with Qemu(cmdline=cmdline, name=name, mem="768M") as vm:
        g = Gui(vm)
        g.start_desktop()
        g.sh("gapterm &")
        g.sleep(3)
        g.move_to(300, 300)
        for i in range(20):
            g.move_to(300 + 10 * i, 300 + 6 * i)
        g.sleep(3)
        return g.sh("cat /tmp/fairyd.stat")


@test
def hwaudit_fairyd_frame_stats():
    """fairyd writes /tmp/fairyd.stat (compose and flush time per frame); fairyd.copy=rep and fairyd.damage=bbox select the old paths."""
    out = _desktop_stat("t-hw-fairyd", "text")
    assert "copy: nt, damage: list" in out, out
    m = re.search(r"frames (\d+), rectangles (\d+)", out)
    assert m and int(m.group(1)) > 5, out
    assert re.search(r"compose \d+ us per frame, flush \d+ us per frame", out), out
    out2 = _desktop_stat("t-hw-fairyd-old", "text fairyd.copy=rep fairyd.damage=bbox")
    assert "copy: rep, damage: bbox" in out2, out2
    m2 = re.search(r"frames (\d+), rectangles (\d+)", out2)
    assert m2 and m2.group(1) == m2.group(2), "with one bounding box every frame is exactly one rectangle: " + out2


@test
def hwaudit_stage_marks():
    """Boot progress squares (shared/stagemark.h): with stagehold=6 the loader and the kernel have drawn six white squares in the top-left corner and the machine stops there."""
    with Qemu(cmdline="stagehold=6 exitmarks", name="t-hw-stage") as vm:      # exitmarks: the loader draws squares 2 and 3 too (off by default, see boot/main.c)
        vm.expect(r"loader: entering kernel", timeout=90)
        time.sleep(3)
        shot = vm.screen_ppm()
        centres = [vm.pixel(6 + 20 * i + 7, 13, shot) for i in range(8)]
        white = [all(c > 240 for c in p) for p in centres]
        assert white == [True] * 6 + [False] * 2, f"squares drawn: {white}, pixels {centres}"
        assert "[Gensokyo] kernel" not in vm.text(), "the kernel went on after stagehold=6"


@test
def hwaudit_loader_ladder():
    """The loader's ladder of workarounds (strategy=N): rung 0 restarts the machine by itself after the exit call came back, the others boot to the shell, the stdmem rungs hand the kernel and the initrd over as such."""
    with Qemu(cmdline="text strategy=0", name="t-hw-rung0") as vm:
        vm.expect(r"loader: rung 0 of 9", timeout=90)
        assert vm.wait_exit(180) is not None, "rung 0 should restart the machine by itself"
        assert "[Gensokyo] kernel" not in vm.text(), "the kernel must not start on rung 0"
    for rung in (1, 2, 4):
        with Qemu(cmdline=f"text strategy={rung}", name=f"t-hw-rung{rung}") as vm:
            vm.expect(PROMPT, timeout=120)
            text = vm.text()
            assert f"loader: rung {rung} of 9" in text, text[-800:]
            if rung >= 2:
                assert "stdmem" in text and "reclaimed" in text, text[-800:]


@test
def hwaudit_panic_on_desktop():
    """A kernel panic while the desktop owns the screen is drawn: killing init as root stops the compositor's picture and shows the incident text (red) on the console."""
    with Qemu(cmdline="autologin=root", name="t-hw-panic-desktop", mem="768M") as vm:
        vm.expect(r"opening the desktop for", timeout=120)
        deadline = time.time() + 40
        while time.time() < deadline:                                       # the panel's start button is shrine red once the desktop is up
            shot = vm.screen_ppm()
            if all(abs(a - b) <= 60 for a, b in zip(vm.pixel(118, 756, shot), (0xB5, 0x12, 0x2B))):
                break
            time.sleep(0.5)
        else:
            raise AssertionError("the desktop never came up:\n" + vm.text()[-600:])
        for _ in range(3):                                                  # the terminal may need a second try on a busy host
            vm.key("t", ctrl=True, alt=True)
            time.sleep(4)
            vm.type("kill -9 1\n")
            try:
                vm.expect(r"KERNEL PANIC", timeout=25)
                break
            except Exception:                                               # noqa: BLE001
                continue
        vm.expect(r"the shrine has no maiden", timeout=30)
        time.sleep(1.5)
        shot = vm.screen_ppm()
        w, h = 1024, 768
        red = sum(1 for x in range(0, 400, 2) for y in range(0, h, 2)
                  if all(abs(a - b) <= 30 for a, b in zip(vm.pixel(x, y, shot), (239, 41, 41))))
        assert red > 60, f"no red incident text on the screen ({red} pixels); the panic is invisible while the desktop runs"
        start = vm.pixel(118, 756, shot)
        assert not all(abs(a - b) <= 60 for a, b in zip(start, (0xB5, 0x12, 0x2B))), f"the desktop is still on screen: {start}"
