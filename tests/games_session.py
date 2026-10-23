#!/usr/bin/env python3
"""Like gui_session.py, with the extras a game needs: waiting for a line of the program's output instead of a fixed time,
holding keys down, and sampling the screen.

  python tests/games_session.py script.txt [out-prefix]

Script lines (in addition to those of gui_session.py: plain lines are typed into the shell; @wait @shot @key @move @click
@down @up @type @print):
  @expect SECONDS REGEX        wait until the serial output (after the previous match) matches; fails the script after the timeout
  @hold QCODE SECONDS [mods]   press, hold, release (mods: shift ctrl alt)
  @press QCODE [mods]          press and keep holding
  @release QCODE
  @clock                       print the guest's uptime next to the host time (the guest clock can run slow when the host
                               timer resolution is coarse; use @expect rather than @wait for anything the guest must have done)
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD  # noqa: E402


def run(script_lines, prefix, cmdline="text", mem="768M"):
    with Qemu(cmdline=cmdline, name="t-games", mem=mem) as vm:
        vm.expect(r"reimu@gensokyo \S+ \$ ", timeout=120)
        for line in script_lines:
            if not line.strip() or line.startswith("#"):
                continue
            if line.startswith("@wait"):
                time.sleep(float(line.split()[1]))
            elif line.startswith("@expect"):
                _, secs, pattern = line.split(None, 2)
                vm.expect(pattern, timeout=float(secs))
                print("matched", pattern)
            elif line.startswith("@shot"):
                p = vm.screenshot(BUILD / f"{prefix}-{line.split()[1]}.png")
                print("screenshot", p)
            elif line.startswith("@hold"):
                parts = line.split()
                vm.key(parts[1], hold=float(parts[2]), shift="shift" in parts[3:], ctrl="ctrl" in parts[3:], alt="alt" in parts[3:])
            elif line.startswith("@press"):
                parts = line.split()
                vm._input([vm._key_event(parts[1], True)], vm.input_device)
            elif line.startswith("@release"):
                parts = line.split()
                vm._input([vm._key_event(parts[1], False)], vm.input_device)
            elif line.startswith("@key"):
                parts = line.split()
                vm.key(parts[1], shift="shift" in parts[2:], ctrl="ctrl" in parts[2:], alt="alt" in parts[2:])
            elif line.startswith("@move"):
                _, dx, dy = line.split()
                vm.mouse_move(int(dx), int(dy))
            elif line.startswith("@down") or line.startswith("@up"):
                parts = line.split()
                vm.mouse_button(parts[1] if len(parts) > 1 else "left", line.startswith("@down"))
                time.sleep(0.15)
            elif line.startswith("@click"):
                parts = line.split()
                vm.mouse_click(parts[1] if len(parts) > 1 else "left")
            elif line.startswith("@type"):
                vm.type(line[len("@type "):])
            elif line.startswith("@clock"):
                vm.send("cat /proc/uptime\n")
                time.sleep(1.0)
                print("host time", time.time(), "serial tail:", vm.text()[-60:].strip().splitlines()[-2:])
            elif line.startswith("@print"):
                print(vm.text()[-3000:])
            else:
                vm.send(line + "\n")
                time.sleep(0.4)
        print(vm.text()[-1500:])


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    script = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    run(script, sys.argv[2] if len(sys.argv) > 2 else "games")
