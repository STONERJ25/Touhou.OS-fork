#!/usr/bin/env python3
"""Runs a command while it holds one of a few shared "QEMU slots", so that parallel agents do not run more QEMU guests than this PC can host.

  python tools/qemu-slot.py [--slots 2] [--wait 7200] -- python tests/run_all.py update_

A guest slows down 2 to 5 times when the PC is overloaded, and timing-dependent tests then fail for no reason of their own: with a dozen agents at work, every QEMU test run (tests/run_all.py,
tools/suite.py, anything that starts qemu-system-x86_64) goes through this wrapper. Host tests (tools/hosttest.py) and builds do not need it.

The slots are lock files in <main checkout>/build/qemu-slots/ (found through `git rev-parse --git-common-dir`, so every worktree of the repository shares them). A holder refreshes its file
every 15 seconds; a file nobody refreshed for 90 seconds belongs to a process that died and is taken over. The command's exit status is returned.
"""
import argparse
import atexit
import os
import subprocess
import sys
import threading
import time
from pathlib import Path


def slots_dir():
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True, text=True, cwd=Path.cwd())
    base = (Path.cwd() / common.stdout.strip()).resolve().parent if common.returncode == 0 and common.stdout.strip() else Path(__file__).resolve().parent.parent
    d = base / "build" / "qemu-slots"
    d.mkdir(parents=True, exist_ok=True)
    return d


def acquire(d, slots, wait, label):
    start, last_said = time.time(), 0.0
    while True:
        for i in range(slots):
            f = d / f"slot{i}.lock"
            try:
                if f.exists() and time.time() - f.stat().st_mtime > 90:
                    f.unlink()                                          # a holder that died
                fd = os.open(f, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, f"{os.getpid()} {time.strftime('%H:%M:%S')} {label}\n".encode())
                os.close(fd)
                return f
            except (FileExistsError, PermissionError, FileNotFoundError):
                continue
        if time.time() - start > wait:
            sys.exit("qemu-slot: no slot became free in time")
        if time.time() - last_said > 60:
            holders = []
            for i in range(slots):
                try:
                    holders.append((d / f"slot{i}.lock").read_text().strip())
                except OSError:
                    pass
            print(f"qemu-slot: waiting for a free slot ({slots} in use: {'; '.join(holders)})", flush=True)
            last_said = time.time()
        time.sleep(5)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--" not in argv:
        print(__doc__)
        return 2
    k = argv.index("--")
    ap = argparse.ArgumentParser()
    ap.add_argument("--slots", type=int, default=2)
    ap.add_argument("--wait", type=int, default=7200)
    opts, cmd = ap.parse_args(argv[:k]), argv[k + 1:]
    if not cmd:
        print(__doc__)
        return 2
    d = slots_dir()
    lock = acquire(d, opts.slots, opts.wait, " ".join(cmd)[:80])
    stop = threading.Event()

    def beat():
        while not stop.wait(15):
            try:
                os.utime(lock)
            except OSError:
                pass

    threading.Thread(target=beat, daemon=True).start()

    def release():
        stop.set()
        try:
            lock.unlink()
        except OSError:
            pass

    atexit.register(release)
    try:
        return subprocess.call(cmd)
    finally:
        release()


if __name__ == "__main__":
    sys.exit(main())
