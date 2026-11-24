#!/usr/bin/env python3
"""Measures the first connection to a fresh guest's sshd: the time from connect() to the server's banner, for three kinds of client, each in its own boot.

  python tools/qemu-slot.py -- python tools/net-stall-probe.py [--modes speak,silent,delayed] [--runs 2] [--pcap build/stall] [--cmdline "..."] [--port 22]

  speak     connects and writes "SSH-2.0-probe\\r\\n" at once (what every ssh client does): the kind that showed the 11 to 14 second stall
  silent    connects and waits for the banner without sending
  delayed   waits 30 seconds after sshd listens, then behaves like `speak`
  second    (not in the default list) a `silent` connection first, then a `speak` one: is it the first connection or the first speak-first one?

Each line of the result: mode, run, seconds from connect() to the first banner byte, seconds the connect() itself took. With --pcap PREFIX every boot writes
PREFIX-<mode>-<run>.pcap (the harness's TOUHOU_PCAP option); `python tools/pcapdump.py FILE --tcp 22` prints the frames. docs/NETWORK.md, "The first-connection stall".
"""
import argparse
import os
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))


def banner_time(port, speak, limit=40.0):
    """(seconds to the banner or None, seconds of the connect, the banner line)"""
    t0 = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", port), timeout=limit)
    t1 = time.perf_counter()
    try:
        if speak:
            s.sendall(b"SSH-2.0-probe\r\n")
        s.settimeout(limit)
        try:
            data = s.recv(256)
        except (socket.timeout, OSError) as e:
            return None, t1 - t0, repr(e)
        return time.perf_counter() - t1, t1 - t0, data.split(b"\n")[0].decode(errors="replace").strip()
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--modes", default="speak,silent,delayed")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--pcap", help="prefix of the capture files (one per boot)")
    ap.add_argument("--cmdline", default="sshd.throttle=0", help="extra kernel command line words")
    ap.add_argument("--delay", type=float, default=30.0, help="the wait of the `delayed` mode, seconds")
    ap.add_argument("--dmesg", action="store_true", help="print the guest's sshd and network log lines after each measurement")
    args = ap.parse_args()
    results = []
    for mode in args.modes.split(","):
        for run in range(1, args.runs + 1):
            if args.pcap:
                os.environ["TOUHOU_PCAP"] = f"{args.pcap}-{mode}-{run}.pcap"
            from case_ssh import SshGuest                           # (after TOUHOU_PCAP: the harness reads it at each start anyway)
            t_boot = time.perf_counter()
            with SshGuest(f"stall-{mode}", cmdline_extra=args.cmdline) as g:
                listening = time.perf_counter() - t_boot
                if mode == "delayed":
                    time.sleep(args.delay)
                if mode == "second":
                    first = banner_time(g.port, speak=False)
                    print(f"  (second: the silent connection before it: banner after {first[0]!r} s)", flush=True)
                took, conn, line = banner_time(g.port, speak=(mode != "silent"))
                again = banner_time(g.port, speak=True)
                shown = "no banner in 40 s" if took is None else f"{took:6.2f} s"
                print(f"{mode:8s} run {run}: banner after {shown} (connect {conn * 1000:.0f} ms, listening {listening:.0f} s after QEMU started; "
                      f"the next speak-first connection: {again[0] if again[0] is None else round(again[0], 2)} s)  {line}", flush=True)
                results.append((mode, run, took))
                if args.dmesg:
                    print(g.out("dmesg | grep -i -e sshd -e tcp -e arp | tail -30"), flush=True)
    print("\nsummary (seconds from connect to banner):")
    for mode in args.modes.split(","):
        print(f"  {mode:8s} " + "  ".join("none" if t is None else f"{t:.2f}" for m, _, t in results if m == mode))


if __name__ == "__main__":
    main()
