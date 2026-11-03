#!/usr/bin/env python3
"""Runs the host tests (tests/host: the x86 CPU core, the PC-98 library, the push tools) where a C compiler and the C library headers exist.

  python tools/hosttest.py discover -s tests/host -p "test_x86*.py"      (the arguments are python -m unittest's)
  python tools/hosttest.py discover -s tests/host -p "test_pc98.py" -v
  python tools/hosttest.py tests.host.test_machine_tools

On Linux and macOS that is the machine itself. On Windows it is **WSL's Ubuntu**: this PC has clang and nasm but none of the Windows C library headers (no Windows SDK, no Visual Studio), so a native compile
fails at <string.h>; the Ubuntu in WSL has gcc 13 and python3, and sees the repository under /mnt/c. nasm comes from the Windows install through tests/host/nasm-wsl.sh. The tools are built into
build/host-x86-wsl (Linux binaries), apart from any native build.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main(argv):
    args = argv or ["discover", "-s", "tests/host", "-p", "test_*.py"]
    if os.name != "nt":
        return subprocess.call([sys.executable, "-m", "unittest", *args], cwd=ROOT)
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        print("hosttest: no wsl.exe: install WSL with a distribution that has gcc, or run the tests on Linux", file=sys.stderr)
        return 2
    drive = ROOT.drive[:1].lower()
    wsl_root = f"/mnt/{drive}/" + str(ROOT)[3:].replace("\\", "/")
    quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in args)
    script = f"cd '{wsl_root}' && TOUHOU_NASM=\"$PWD/tests/host/nasm-wsl.sh\" PYTHONUNBUFFERED=1 python3 -m unittest {quoted}"
    return subprocess.call([wsl, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "sh", "-c", script])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
