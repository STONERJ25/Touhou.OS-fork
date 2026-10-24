#!/usr/bin/env python3
"""QEMU launcher and scripted test harness for Gensokyo.

  python tools/qemu.py run  [--cmdline TEXT] [--mem 512M] [--net] [--no-disk]   interactive window + serial on stdio
  python tools/qemu.py boot [--until REGEX] [--keys TEXT] [--shot PNG] ...       headless boot, print serial, exit

As a library (tests/*.py):
  from qemu import Qemu
  with Qemu(cmdline="quiet") as vm:
      vm.expect(r"gapsh> ")
      vm.type("ls\\n")
      vm.expect(r"bin")
      vm.screenshot("build/shot.png")
"""
import argparse
import codecs
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


# Where QEMU and the UEFI firmware live. Each comes from, in order: an environment variable (TOUHOU_QEMU = the
# qemu-system-x86_64 program, TOUHOU_OVMF_CODE / TOUHOU_OVMF_VARS = the two firmware images), the PATH (for QEMU), and
# then the usual places: the Windows installer's folder first (what this was written on), then the Linux packages.
# docs/LINUX.md has the details.
WINDOWS_QEMU_DIR = Path(r"C:\Program Files\qemu")
# (code, vars) pairs that belong together; the first pair whose files both exist wins.
OVMF_PAIRS = [
    ("{qemu}/share/edk2-x86_64-code.fd", "{qemu}/share/edk2-i386-vars.fd"),        # QEMU for Windows (and a QEMU built from source)
    ("{qemu}/../share/qemu/edk2-x86_64-code.fd", "{qemu}/../share/qemu/edk2-i386-vars.fd"),
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),            # Debian / Ubuntu (package ovmf)
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),                  # older Debian / Ubuntu
    ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),    # Arch (edk2-ovmf)
    ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),        # Fedora (edk2-ovmf)
    ("/usr/share/qemu/edk2-x86_64-code.fd", "/usr/share/qemu/edk2-i386-vars.fd"),
]


def find_qemu():
    env = os.environ.get("TOUHOU_QEMU")
    if env:
        return Path(env)
    found = shutil.which("qemu-system-x86_64")
    if found:
        return Path(found)
    return WINDOWS_QEMU_DIR / "qemu-system-x86_64.exe"


def find_ovmf(qemu):
    """Returns (code, vars). Missing files are reported when a VM starts, not at import time, so that host-only users of
    this module (tests that only build disk images) work without QEMU."""
    code, vars_ = os.environ.get("TOUHOU_OVMF_CODE"), os.environ.get("TOUHOU_OVMF_VARS")
    if code and vars_:
        return Path(code), Path(vars_)
    qdir = str(Path(qemu).resolve().parent) if Path(qemu).exists() else str(WINDOWS_QEMU_DIR)
    for c, v in OVMF_PAIRS:
        c, v = Path(c.format(qemu=qdir)), Path(v.format(qemu=qdir))
        if c.is_file() and v.is_file():
            return Path(code) if code else c, Path(vars_) if vars_ else v
    c, v = OVMF_PAIRS[0]
    return Path(code or c.format(qemu=qdir)), Path(vars_ or v.format(qemu=qdir))


QEMU = find_qemu()
OVMF_CODE, OVMF_VARS = find_ovmf(QEMU)

# TOUHOU_TIMEOUT_SCALE=3 multiplies every timeout of the harness by 3, for slow hosts (QEMU without KVM/WHPX, a busy CI
# runner). The guest itself is not slowed down or sped up; only the waiting gets more patient. Default 1: no change.
try:
    TIMEOUT_SCALE = max(1.0, float(os.environ.get("TOUHOU_TIMEOUT_SCALE", "1") or 1))
except ValueError:
    TIMEOUT_SCALE = 1.0


def scaled(seconds):
    """A timeout in seconds, stretched by TOUHOU_TIMEOUT_SCALE."""
    return seconds * TIMEOUT_SCALE


def check_tools():
    for what, path, var in (("QEMU", QEMU, "TOUHOU_QEMU"), ("UEFI firmware (code)", OVMF_CODE, "TOUHOU_OVMF_CODE"),
                            ("UEFI firmware (vars)", OVMF_VARS, "TOUHOU_OVMF_VARS")):
        if not Path(path).is_file():
            raise RuntimeError(f"{what} not found at {path}: install it or set {var} (see docs/LINUX.md)")


_ANSI = re.compile(r"\x1b\[[0-9;?=]*[A-Za-z]|\x1b[()][A-Za-z0-9]|\x1b[=>]")
_PARTIAL_ESC = re.compile(r"\x1b(\[[0-9;?=]*)?$")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# qcode names for QMP input-send-event
_PLAIN = {
    " ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", "\x1b": "esc",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right", ";": "semicolon",
    "'": "apostrophe", "`": "grave_accent", "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
}
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", ":": ";", '"': "'", "~": "`", "|": "\\", "<": ",", ">": ".", "?": "/",
}


def char_to_key(ch):
    """Returns (qcode, needs_shift)."""
    if ch.isalpha() and ch.isascii():
        return ch.lower(), ch.isupper()
    if ch.isdigit():
        return ch, False
    if ch in _PLAIN:
        return _PLAIN[ch], False
    if ch in _SHIFTED:
        return _PLAIN.get(_SHIFTED[ch], _SHIFTED[ch]), True
    raise ValueError(f"no key for {ch!r}")


class Qemu:
    def __init__(self, cmdline="", mem="512M", display="none", disk=None, net=False, no_reboot=True,
                 trace_int=False, extra=(), esp=None, name="vm", video=None, headless_serial=True,
                 boot_image=None, boot_bus="usb", audio=None, audio_path=None, sounds=False, hostfwd=(), netdev_opts=""):
        # audio: None (no sound hardware), "hda" / "hda-output" (Intel HDA + output-only codec), "hda-duplex", "hda-micro", "hda-nocodec", "ac97".
        # The sound card plays into a WAV file (audio_path, default build/<name>-audio.wav) that is complete once stop() ran.
        # sounds: system sounds (login chime...) are switched off with "nosounds" on the command line unless a test wants them.
        self.cmdline, self.mem, self.display = cmdline, mem, display
        self.audio, self.sounds = audio, sounds
        self.audio_path = Path(audio_path) if audio_path else BUILD / f"{name_safe(name)}-audio.wav"
        self.disk, self.net, self.no_reboot, self.trace_int = disk, net, no_reboot, trace_int
        self.hostfwd = list(hostfwd)
        self.netdev_opts = netdev_opts
        self.extra = list(extra)
        self.esp = Path(esp) if esp else BUILD / "esp"
        self.name = name
        self.video = video
        self.boot_image, self.boot_bus = boot_image, boot_bus    # boot from a disk image (as a PC would) instead of the fat: ESP directory
        self.input_device = None        # QMP id of the keyboard to inject keys into (e.g. a usb-kbd); None = QEMU's default
        self.mouse_device = None        # same for the mouse
        self.proc = None
        self.serial = ""
        self._pos = 0
        self._lock = threading.Lock()
        self._qmp = None
        self._qmp_file = None
        self._serial_sock = None
        self.log_path = BUILD / f"{name}-qemu.log"
        self.serial_path = BUILD / f"{name}-serial.log"

    # ------------------------------------------------------------ lifecycle
    def _prepare(self):
        check_tools()
        BUILD.mkdir(exist_ok=True)
        vars_copy = BUILD / f"{name_safe(self.name)}-vars.fd"
        if not vars_copy.exists():
            vars_copy.write_bytes(OVMF_VARS.read_bytes())
        cmdline = self.cmdline
        if not self.sounds and "nosounds" not in cmdline.split():
            cmdline = (cmdline + " nosounds").strip()
        if self.audio and self.audio_path.exists():
            self.audio_path.unlink()
        if self.video:
            cmdline = (cmdline + f" video={self.video}").strip()
        (self.esp / "cmdline.txt").write_text(cmdline + "\n")
        return vars_copy

    def _boot_drive_args(self):
        """The firmware's boot disk: the ESP directory as a FAT drive, or a whole disk image on a USB stick, SATA, NVMe or virtio bus."""
        if not self.boot_image:
            return ["-drive", f"format=raw,file=fat:rw:{self.esp}"]
        img = f"if=none,id=bootdisk,format=raw,file={self.boot_image}"
        bus = self.boot_bus
        if bus == "usb":
            return ["-drive", img, "-device", "qemu-xhci,id=bootxhci", "-device", "usb-storage,bus=bootxhci.0,drive=bootdisk,bootindex=0"]
        if bus == "sata":
            return ["-drive", img, "-device", "ide-hd,drive=bootdisk,bootindex=0"]
        if bus == "nvme":
            return ["-drive", img, "-device", "nvme,drive=bootdisk,serial=bootnvme,bootindex=0"]
        if bus == "virtio":
            return ["-drive", img, "-device", "virtio-blk-pci,drive=bootdisk,bootindex=0"]
        raise ValueError(f"unknown boot bus {bus!r}")

    def _net_args(self):
        """The QEMU options for the guest's network card.

        net=False             no card (and no -netdev), as before
        net=True              an e1000 on user-mode networking (10.0.2.0/24, gateway 10.0.2.2, DNS 10.0.2.3), as before
        net="virtio-net-pci"  any NIC model QEMU knows: e1000, e1000e, rtl8139, virtio-net-pci[,disable-legacy=on] ...
        hostfwd=[...]         user-mode port forwards, each "tcp:127.0.0.1:18080-:80" (host side first, guest side after the dash)
        netdev_opts="..."     extra -netdev user options such as "restrict=on" or "net=192.168.7.0/24"
        """
        if not self.net:
            return ["-net", "none"]
        model = "e1000" if self.net is True else str(self.net)
        netdev = "user,id=net0"
        for fwd in self.hostfwd:
            netdev += f",hostfwd={fwd}"
        if self.netdev_opts:
            netdev += "," + self.netdev_opts
        return ["-netdev", netdev, "-device", f"{model},netdev=net0"]

    def _args(self, vars_copy, serial_port, qmp_port):
        a = [str(QEMU),
             "-machine", "q35", "-cpu", os.environ.get("TOUHOU_CPU", "max"), "-m", self.mem, "-smp", "1",    # TOUHOU_CPU=Skylake-Client-noTSX-IBRS: closer to an i5-7500
             "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
             "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
             *self._boot_drive_args(),
             "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-d", "guest_errors,cpu_reset" + (",int" if self.trace_int else ""), "-D", str(self.log_path)]
        if self.disk:
            a += ["-drive", f"if=none,id=data0,format=raw,file={self.disk}", "-device", "virtio-blk-pci,drive=data0"]
        a += self._net_args()
        if self.audio:
            a += ["-audiodev", f"wav,id=snd0,path={str(self.audio_path).replace(chr(92), '/')},out.frequency=48000,out.channels=2,out.format=s16"]
            if self.audio == "ac97":
                a += ["-device", "AC97,audiodev=snd0"]
            else:
                a += ["-device", "intel-hda,id=hda0"]
                if self.audio != "hda-nocodec":                        # "hda-nocodec": the controller alone, nobody answers on the link
                    codec = self.audio if self.audio in ("hda-duplex", "hda-micro") else "hda-output"
                    a += ["-device", f"{codec},bus=hda0.0,audiodev=snd0"]
        if self.no_reboot:
            a += ["-no-reboot"]
        if self.display:
            a += ["-display", self.display]
        if serial_port:
            a += ["-serial", f"tcp:127.0.0.1:{serial_port},server=on,wait=on"]
        if qmp_port:
            a += ["-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off"]
        return a + self.extra

    def start(self):
        """Starts QEMU; retries if startup fails (a racing free port or a transient Windows hiccup)."""
        last = None
        for attempt in range(3):
            try:
                self.serial = ""
                self._pos = 0
                return self._start_once()
            except RuntimeError as e:
                last = e
                self.stop()
                time.sleep(0.5)
        raise last

    def _start_once(self):
        vars_copy = self._prepare()
        sp, qp = free_port(), free_port()
        args = self._args(vars_copy, sp, qp)
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if self.display == "none" else 0     # Windows only: no console window
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags)
        deadline = time.time() + scaled(15)
        while True:
            try:
                self._serial_sock = socket.create_connection(("127.0.0.1", sp), timeout=1)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("QEMU exited at startup:\n" + self.proc.stderr.read().decode(errors="replace"))
                if time.time() > deadline:
                    raise RuntimeError("could not connect to QEMU serial port")
                time.sleep(0.05)
        self._serial_sock.settimeout(None)
        self.serial_path.write_bytes(b"")
        threading.Thread(target=self._serial_reader, daemon=True).start()
        while True:
            try:
                self._qmp = socket.create_connection(("127.0.0.1", qp), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError("could not connect to QMP")
                time.sleep(0.05)
        self._qmp_file = self._qmp.makefile("rw", encoding="utf-8", newline="\n")
        self._qmp_file.readline()   # greeting
        self.qmp("qmp_capabilities")
        return self

    def _serial_reader(self):
        """Collects serial output: raw bytes go to the log file, a UTF-8 decoded copy with ANSI codes stripped
        goes to self.serial (what expect() matches against)."""
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        pending = ""
        with open(self.serial_path, "ab", buffering=0) as log:
            while True:
                try:
                    data = self._serial_sock.recv(4096)
                except OSError:
                    break
                if not data:
                    break
                log.write(data)
                pending += decoder.decode(data)
                m = _PARTIAL_ESC.search(pending)        # hold back an escape sequence split across reads
                hold = ""
                if m:
                    hold = pending[m.start():]
                    pending = pending[:m.start()]
                clean = _ANSI.sub("", pending).replace("\r", "")
                pending = hold
                with self._lock:
                    self.serial += clean

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_exit(self, timeout=30):
        """Waits for QEMU to exit by itself (isa-debug-exit); returns the guest's exit code or None on timeout."""
        try:
            rc = self.proc.wait(timeout=scaled(timeout))
        except subprocess.TimeoutExpired:
            return None
        time.sleep(0.2)
        return (rc >> 1) if rc & 1 else rc

    def stop(self):
        if self._qmp_file and self.alive():
            try:
                self.qmp("quit")
            except Exception:
                pass
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        for s in (self._qmp, self._serial_sock):
            try:
                if s:
                    s.close()
            except OSError:
                pass

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # ------------------------------------------------------------ serial
    def text(self):
        with self._lock:
            return self.serial

    def expect(self, pattern, timeout=30, flags=re.S):
        """Waits for pattern in serial output after the previous match; returns the match object."""
        rx = re.compile(pattern, flags)
        end = time.time() + scaled(timeout)
        while True:
            with self._lock:
                m = rx.search(self.serial, self._pos)
                if m:
                    self._pos = m.end()
                    return m
            if not self.alive():
                time.sleep(0.2)     # let the reader drain
                with self._lock:
                    m = rx.search(self.serial, self._pos)
                    if m:
                        self._pos = m.end()
                        return m
                raise RuntimeError(f"QEMU exited (code {self.proc.returncode}) while waiting for /{pattern}/\n" + self.diagnose())
            if time.time() > end:
                raise TimeoutError(f"timeout waiting for /{pattern}/\n" + self.diagnose())
            time.sleep(0.02)

    def diagnose(self):
        out = ["--- serial tail ---", self.text()[-3000:]]
        if self.log_path.exists():
            log = self.log_path.read_text(errors="replace")
            out += ["--- qemu log tail ---", log[-2500:]]
        return "\n".join(out)

    # ------------------------------------------------------------ QMP
    def qmp(self, cmd, **args):
        msg = {"execute": cmd}
        if args:
            msg["arguments"] = args
        self._qmp_file.write(json.dumps(msg) + "\n")
        self._qmp_file.flush()
        while True:
            line = self._qmp_file.readline()
            if not line:
                return None
            obj = json.loads(line)
            if "return" in obj:
                return obj["return"]
            if "error" in obj:
                raise RuntimeError(f"QMP {cmd}: {obj['error']}")

    def _key_event(self, qcode, down):
        return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}

    def _input(self, events, device=None):
        """input-send-event. `device` is accepted for compatibility but NOT forwarded: QEMU's `device` argument names a *display*
        device (a console), not an input device, and QEMU 11 aborts on any id that is not a display ("Property
        'qemu-fixed-text-console.device' not found"). Default routing already does the right thing for USB input: a usb-kbd becomes
        the default keyboard when it is plugged in, and a usb-mouse the default mouse once the guest starts polling it (check
        with select_mouse() / query-mice)."""
        self.qmp("input-send-event", events=events)

    def select_mouse(self, name):
        """Makes the mouse whose QEMU name contains `name` ("HID", "PS/2") the one that receives injected motion and buttons."""
        for m in self.qmp("query-mice"):
            if name.lower() in m["name"].lower():
                self.qmp("human-monitor-command", **{"command-line": f"mouse_set {m['index']}"})
                return m["index"]
        raise RuntimeError(f"no mouse matching {name!r}")

    def key(self, qcode, shift=False, ctrl=False, alt=False, hold=0.03):
        mods = (["shift"] if shift else []) + (["ctrl"] if ctrl else []) + (["alt"] if alt else [])
        self._input([self._key_event(m, True) for m in mods] + [self._key_event(qcode, True)], self.input_device)
        time.sleep(hold)
        self._input([self._key_event(qcode, False)] + [self._key_event(m, False) for m in reversed(mods)], self.input_device)
        time.sleep(hold)

    def type(self, text, delay=0.02):
        for ch in text:
            q, shift = char_to_key(ch)
            self.key(q, shift=shift, hold=delay)

    def send(self, text):
        """Types text into the guest over the serial port (reaches the console tty like a keyboard would)."""
        self._serial_sock.sendall(text.encode("utf-8"))

    def mouse_move(self, dx, dy):
        self._input([
            {"type": "rel", "data": {"axis": "x", "value": int(dx)}},
            {"type": "rel", "data": {"axis": "y", "value": int(dy)}}], device=self.mouse_device)

    def mouse_button(self, button="left", down=True):
        self._input([{"type": "btn", "data": {"down": down, "button": button}}], device=self.mouse_device)

    def mouse_click(self, button="left"):
        self.mouse_button(button, True)
        time.sleep(0.05)
        self.mouse_button(button, False)
        time.sleep(0.05)

    def screen_ppm(self):
        """Returns (width, height, rgb_bytes) of the current screen, for tests that inspect pixels."""
        path = BUILD / f"{name_safe(self.name)}-screen.ppm"
        path.parent.mkdir(parents=True, exist_ok=True)
        self.qmp("screendump", filename=str(path).replace("\\", "/"), format="ppm")
        data = path.read_bytes()
        parts = data.split(b"\n", 3)                    # P6 / W H / 255 / pixels
        w, h = map(int, parts[1].split())
        return w, h, parts[3]

    def pixel(self, x, y, shot=None):
        w, h, rgb = shot or self.screen_ppm()
        i = (y * w + x) * 3
        return rgb[i], rgb[i + 1], rgb[i + 2]

    def screenshot(self, path):
        path = Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.qmp("screendump", filename=str(path).replace("\\", "/"), format="png")
        return path


def name_safe(s):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", s)


# ---------------------------------------------------------------------- CLI

def cmd_run(args):
    check_tools()
    BUILD.mkdir(exist_ok=True)
    vars_copy = BUILD / "vars.fd"
    if not vars_copy.exists():
        vars_copy.write_bytes(OVMF_VARS.read_bytes())
    esp = BUILD / "esp"
    cmdline = args.cmdline + (f" video={args.video}" if args.video else "")
    (esp / "cmdline.txt").write_text(cmdline.strip() + "\n")
    disk = None if args.no_disk else (BUILD / "disk.img")
    q = Qemu(cmdline=args.cmdline, mem=args.mem, display=args.display, disk=disk if disk and disk.exists() else None,
             net=args.net, no_reboot=False, name="run")
    a = q._args(vars_copy, None, None)
    a += ["-serial", "stdio"]
    print("qemu:", " ".join(a))
    sys.exit(subprocess.call(a))


def cmd_boot(args):
    disk = None if args.no_disk else (BUILD / "disk.img")
    with Qemu(cmdline=args.cmdline, mem=args.mem, display="none", disk=disk if disk and disk.exists() else None,
              net=args.net, trace_int=args.trace_int, video=args.video) as vm:
        status = 0
        try:
            if args.until:
                vm.expect(args.until, timeout=args.timeout)
            else:
                time.sleep(args.timeout)
            for k in args.keys or []:
                vm.type(k.encode().decode("unicode_escape"))
                time.sleep(0.3)
            if args.after:
                vm.expect(args.after, timeout=args.timeout)
            elif args.keys:
                time.sleep(args.wait)
            if args.wait and not args.keys:
                time.sleep(args.wait)
            if args.shot:
                if vm.alive():
                    print("screenshot:", vm.screenshot(args.shot))
                else:
                    print("screenshot skipped: VM already exited")
        except (TimeoutError, RuntimeError) as e:
            print(e)
            status = 1
        if not vm.alive():
            rc = vm.proc.returncode
            print(f"vm exited: qemu status {rc} (guest code {(rc >> 1) if rc & 1 else rc})")
        print("=" * 30, "serial output", "=" * 30)
        print(vm.text())
        sys.exit(status)


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("run", cmd_run), ("boot", cmd_boot)):
        p = sub.add_parser(name)
        p.add_argument("--cmdline", default="")
        p.add_argument("--mem", default="512M")
        p.add_argument("--net", action="store_true")
        p.add_argument("--no-disk", action="store_true")
        p.add_argument("--video", help="WxH requested from the firmware, e.g. 1280x800")
        p.set_defaults(fn=fn)
        if name == "run":
            p.add_argument("--display", help="QEMU -display value (default: QEMU's own default)")
        else:
            p.add_argument("--until", help="regex to wait for on the serial port")
            p.add_argument("--after", help="regex to wait for after sending --keys")
            p.add_argument("--keys", action="append", help="text to type (\\n for Enter); repeatable")
            p.add_argument("--timeout", type=float, default=30)
            p.add_argument("--wait", type=float, default=1.0, help="seconds to settle before the screenshot")
            p.add_argument("--shot", help="save a PNG screenshot here")
            p.add_argument("--trace-int", action="store_true", help="log every interrupt to the QEMU log (huge)")
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
