#!/usr/bin/env python3
"""Receives the kernel log of a TouhouOS machine started with netconsole=IP[:PORT] (docs/RELIABILITY.md).

  python tools/netconsole.py [--port 6666] [--bind 0.0.0.0] [--out FILE] [--quiet]

Every datagram is one line of the machine's kernel log (lines of the netconsole's own start with "-- netconsole:"). Each is printed with the
time it arrived and the sender's address, and appended to FILE (default build/netconsole-YYYYMMDD.log, which grows across runs). Ctrl+C
ends it. The machine sends to the address given on its command line, so give it this PC's LAN address; on Windows the firewall may ask
the first time whether Python may receive on the port.

As a library (tests/case_reliability.py):

  r = Receiver(port=0, bind="127.0.0.1")    # a free port; listens in a thread
  r.port                                    # what to put on the guest's command line (QEMU's user network: netconsole=10.0.2.2:PORT)
  r.wait_for(r"KERNEL PANIC", timeout=30)   # the first line that matches, from the beginning
  r.lines()                                 # everything so far, as (arrival time, sender, text)
  r.close()
"""
import argparse
import datetime
import re
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Receiver:
    def __init__(self, port=6666, bind="0.0.0.0", out=None, echo=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, port))
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.out = Path(out) if out else None
        self.echo = echo
        self._lines = []
        self._datagrams = 0
        self._lock = threading.Lock()
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        fh = open(self.out, "a", encoding="utf-8") if self.out else None
        try:
            while not self._stop:
                try:
                    data, addr = self.sock.recvfrom(65535)
                except socket.timeout:
                    continue
                except OSError:
                    break
                now = time.time()
                text = data.decode("utf-8", "replace")
                stamp = datetime.datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                with self._lock:
                    self._datagrams += 1
                    for line in text.rstrip("\n").split("\n"):
                        self._lines.append((now, addr[0], line))
                        if fh:
                            fh.write(f"{stamp} {addr[0]} | {line}\n")
                        if self.echo:
                            print(f"{stamp} {addr[0]} | {line}", flush=True)
                if fh:
                    fh.flush()
        finally:
            if fh:
                fh.close()

    def lines(self):
        with self._lock:
            return list(self._lines)

    def texts(self):
        return [t for _, _, t in self.lines()]

    def datagrams(self):
        with self._lock:
            return self._datagrams

    def wait_for(self, pattern, timeout=30, start=0):
        """The first line (from index `start`) that matches; raises TimeoutError."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            for i, (_, _, t) in enumerate(self.lines()[start:]):
                if rx.search(t):
                    return start + i, t
            if time.time() > end:
                raise TimeoutError(f"no netconsole line matched /{pattern}/ in {timeout} s; got {len(self.lines())} lines, the last: "
                                   + repr(self.texts()[-5:]))
            time.sleep(0.1)

    def close(self):
        self._stop = True
        self._thread.join(timeout=2)
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=6666)
    ap.add_argument("--bind", default="0.0.0.0", help="the address to listen on (default: every one)")
    ap.add_argument("--out", help="the file to append to (default build/netconsole-YYYYMMDD.log)")
    ap.add_argument("--quiet", action="store_true", help="only the file, nothing on the screen")
    args = ap.parse_args()
    out = Path(args.out) if args.out else ROOT / "build" / f"netconsole-{datetime.date.today():%Y%m%d}.log"
    out.parent.mkdir(parents=True, exist_ok=True)
    r = Receiver(args.port, args.bind, out, echo=not args.quiet)
    print(f"netconsole: listening on {args.bind}:{r.port}, writing to {out} (Ctrl+C ends)", file=sys.stderr, flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    r.close()
    print(f"netconsole: {r.datagrams()} datagrams received", file=sys.stderr)


if __name__ == "__main__":
    main()
