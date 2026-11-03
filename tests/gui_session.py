#!/usr/bin/env python3
"""Boot the system, run shell lines, take screenshots. For looking at the desktop while developing it.

  python tests/gui_session.py script.txt [out-prefix]

script.txt lines:
  <anything>          typed into the shell
  @wait 2.5           sleep
  @shot name          screenshot to build/<prefix>-<name>.png
  @key <qcode> [ctrl] [alt] [shift]
  @move dx dy         relative mouse movement
  @click [left|right|middle]
  @type text          type with the (PS/2) keyboard
  @print              print the serial output seen so far
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD  # noqa: E402

script = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
prefix = sys.argv[2] if len(sys.argv) > 2 else "gui"
with Qemu(cmdline="text", name="t-gui", mem="768M") as vm:
    vm.expect(r"reimu@gensokyo \S+ \$ ", timeout=90)
    for line in script:
        if not line.strip() or line.startswith("#"):
            continue
        if line.startswith("@wait"):
            time.sleep(float(line.split()[1]))
        elif line.startswith("@shot"):
            name = line.split()[1]
            p = vm.screenshot(BUILD / f"{prefix}-{name}.png")
            print("screenshot", p)
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
        elif line.startswith("@print"):
            print(vm.text()[-3000:])
        else:
            vm.send(line + "\n")
            time.sleep(0.4)
    print(vm.text()[-1500:])
