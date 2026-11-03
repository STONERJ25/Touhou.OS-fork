#!/usr/bin/env python3
"""Powers the Dell off over ssh and watches how it comes back by a magic packet (Wake on LAN).

  python tools/wake-test.py                          plain poweroff, magic packets from 25 s after the command for 60 s       (the normal use)
  python tools/wake-test.py --packets-after 40 --packets-for 90

If no packet wakes it, somebody has to press the power button: nothing else brings the machine back (a clock alarm was tried and dropped, see docs/POWER_AND_TIME.md).

The machine goes off about five seconds after the command, not later: packets sent earlier than that arrive while it is still running or the moment it is off, and
wake it at once (the second, third and fourth hardware tries of 2026-11-02 were read as "the machine wakes by itself" for exactly that reason). So the packets start 25 s
after the command by default, and the script kills its packet sender when it ends, whatever happens.

When the machine is back (proved by a token in its /tmp being gone, never by port 22, see machine-watch.py) the script reads, read-only, what the new start says woke it:
the ACPI status bits (the power button, a network card's PME) and the card's own wake status, and prints the timeline.
Exit status: 0 it came back, 4 it did not come back in time (somebody has to press the power button), 3 no baseline (the machine does not answer a login), 5 the host key changed.
"""
import argparse
import importlib.util
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("machine_watch", ROOT / "tools" / "machine-watch.py")
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)

MAC = "aa:bb:cc:dd:ee:ff"


def start_status(m):
    """The lines of the new start's log that say what woke it, and the sources they name."""
    rc, out, err = m.ssh('dmesg | grep -a "acpi: wake status\\|wake-up status\\|wall clock"', timeout=60)
    lines = [l for l in out.splitlines() if l.strip()]
    sources = []
    for l in lines:
        g = re.search(r"PM1 status ([0-9a-f]{4}) enable ([0-9a-f]{4}); event block (0x[0-9a-f]+) status ((?:[0-9a-f]{2} ?)+), enable", l)
        if g:
            pm1 = int(g.group(1), 16)
            ev = [int(x, 16) for x in g.group(4).split()]
            if pm1 & 0x0100:
                sources.append("the power button (PM1 PWRBTN_STS)")
            if len(ev) > 13 and ev[13] & 0x20:
                sources.append("a network card's wake event (PME_B0, event block byte 13 = %02x)" % ev[13])
        g = re.search(r"wake-up status at start: WUS ([0-9a-f]{8})", l)
        if g and int(g.group(1), 16) & 2:
            sources.append("the card matched a magic packet (WUS bit 1)")
    return lines, sources


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.185")
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519")
    ap.add_argument("--known-hosts", default=str(ROOT / "assets" / "local" / "dell_known_hosts"))
    ap.add_argument("--mac", default=MAC)
    ap.add_argument("--packets-after", type=float, default=25, metavar="SECONDS", help="start the magic packets this long after the command (default 25)")
    ap.add_argument("--packets-for", type=float, default=60, metavar="SECONDS", help="send them for this long; 0 = none (default 60)")
    ap.add_argument("--wait", type=int, default=300, help="how long to wait for the machine, seconds (default 300)")
    args = ap.parse_args(argv)
    if args.packets_for <= 0:
        ap.error("a poweroff with no packets would leave the machine off until somebody presses its power button: --packets-for must be positive")

    m = mw.Machine(args)
    token = mw.plant(m)
    mw.log(f"baseline: token {token} planted")
    command = "sudo poweroff"
    sender = None
    t0 = time.time()
    try:
        mw.log("running:", command)
        rc, out, err = m.ssh(command, timeout=12)
        mw.log(f"command returned: rc={rc} out={out.strip()[:200]!r}")
        wait = max(0.0, t0 + args.packets_after - time.time())
        mw.log(f"magic packets in {wait:.0f} s, for {args.packets_for:.0f} s")
        time.sleep(wait)
        mw.log("magic packets begin")
        sender = subprocess.Popen([sys.executable, str(ROOT / "tools" / "wol.py"), args.mac, "--to", args.host, "--seconds", str(args.packets_for), "--interval", "0.5"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        result = mw.wait_restart(m, token, time.time() + args.wait, t_cmd=time.time(), give_up_same=None)
        t_back = time.time()
    finally:
        if sender is not None and sender.poll() is None:
            sender.kill()
            mw.log("the packet sender was stopped")
    if result != "restarted":
        mw.log("the machine did not come back in time: somebody has to press its power button")
        return 4
    lines, sources = start_status(m)
    print()
    print(f"back {t_back - t0:.0f} s after the command; the packets began at {args.packets_after:.0f} s")
    for l in lines:
        print("  " + l.strip())
    print("woken by: " + ("; ".join(sources) if sources else "nothing the log names"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
