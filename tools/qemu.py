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
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
QEMU_DIR = Path(r"C:\Program Files\qemu")
QEMU = QEMU_DIR / "qemu-system-x86_64.exe"
OVMF_CODE = QEMU_DIR / "share" / "edk2-x86_64-code.fd"
OVMF_VARS = QEMU_DIR / "share" / "edk2-i386-vars.fd"


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
                 trace_int=False, extra=(), esp=None, name="vm", video=None, headless_serial=True):
        self.cmdline, self.mem, self.display = cmdline, mem, display
        self.disk, self.net, self.no_reboot, self.trace_int = disk, net, no_reboot, trace_int
        self.extra = list(extra)
        self.esp = Path(esp) if esp else BUILD / "esp"
        self.name = name
        self.video = video
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
        BUILD.mkdir(exist_ok=True)
        vars_copy = BUILD / f"{name_safe(self.name)}-vars.fd"
        if not vars_copy.exists():
            vars_copy.write_bytes(OVMF_VARS.read_bytes())
        cmdline = self.cmdline
        if self.video:
            cmdline = (cmdline + f" video={self.video}").strip()
        (self.esp / "cmdline.txt").write_text(cmdline + "\n")
        return vars_copy

    def _args(self, vars_copy, serial_port, qmp_port):
        a = [str(QEMU),
             "-machine", "q35", "-cpu", "max", "-m", self.mem, "-smp", "1",
             "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
             "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
             "-drive", f"format=raw,file=fat:rw:{self.esp}",
             "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-d", "guest_errors,cpu_reset" + (",int" if self.trace_int else ""), "-D", str(self.log_path)]
        if self.disk:
            a += ["-drive", f"if=none,id=data0,format=raw,file={self.disk}", "-device", "virtio-blk-pci,drive=data0"]
        if self.net:
            a += ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]
        else:
            a += ["-net", "none"]
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
        vars_copy = self._prepare()
        sp, qp = free_port(), free_port()
        args = self._args(vars_copy, sp, qp)
        flags = subprocess.CREATE_NO_WINDOW if self.display == "none" else 0
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags)
        deadline = time.time() + 15
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
        with open(self.serial_path, "ab", buffering=0) as log:
            while True:
                try:
                    data = self._serial_sock.recv(4096)
                except OSError:
                    break
                if not data:
                    break
                log.write(data)
                with self._lock:
                    self.serial += data.decode("utf-8", errors="replace").replace("\r", "")

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

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
        end = time.time() + timeout
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

    def key(self, qcode, shift=False, ctrl=False, alt=False, hold=0.03):
        mods = (["shift"] if shift else []) + (["ctrl"] if ctrl else []) + (["alt"] if alt else [])
        self.qmp("input-send-event", events=[self._key_event(m, True) for m in mods] + [self._key_event(qcode, True)])
        time.sleep(hold)
        self.qmp("input-send-event", events=[self._key_event(qcode, False)] + [self._key_event(m, False) for m in reversed(mods)])
        time.sleep(hold)

    def type(self, text, delay=0.02):
        for ch in text:
            q, shift = char_to_key(ch)
            self.key(q, shift=shift, hold=delay)

    def mouse_move(self, dx, dy):
        self.qmp("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": int(dx)}},
            {"type": "rel", "data": {"axis": "y", "value": int(dy)}}])

    def mouse_button(self, button="left", down=True):
        self.qmp("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": button}}])

    def mouse_click(self, button="left"):
        self.mouse_button(button, True)
        time.sleep(0.05)
        self.mouse_button(button, False)
        time.sleep(0.05)

    def screenshot(self, path):
        path = Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.qmp("screendump", filename=str(path).replace("\\", "/"), format="png")
        return path


def name_safe(s):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", s)


# ---------------------------------------------------------------------- CLI

def cmd_run(args):
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
                print("screenshot:", vm.screenshot(args.shot))
        except (TimeoutError, RuntimeError) as e:
            print(e)
            status = 1
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
