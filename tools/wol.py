#!/usr/bin/env python3
"""Sends a Wake-on-LAN "magic packet" to a machine that is switched off (docs/the project plan, track W).

  python tools/wol.py aa:bb:cc:dd:ee:ff                                 the Dell's network card (the packet goes out for 20 seconds)
  python tools/wol.py aa:bb:cc:dd:ee:ff --seconds 5 --broadcast 192.168.1.255 --bind 192.168.1.15
  python tools/wol.py aa:bb:cc:dd:ee:ff --via me@jump.example --bind 192.168.1.5     from another PC on the machine's network, over ssh (docs/REMOTE_UPDATE.md)

The magic packet is 6 bytes of FF followed by the machine's MAC address 16 times; the card's wake-up filter looks for that pattern in the payload of any frame, so a UDP broadcast does (port 9 "discard", also 7).
It is sent to the limited broadcast address and to the subnet's, from the dev PC's address on the machine's network: on a PC with several adapters (VPNs, Tailscale) the operating system would otherwise pick
the wrong one for a broadcast (a VPN with a default route sent the first packets of 2026-11-03 into the tunnel). --bind names the address to send from; by default the packets go out from **every** address of this PC that
lies in the same /24 as --to (found from the adapters' addresses, and from the route's choice as a last resort), each to its own subnet's broadcast too.

--via [USER@]HOST[:PORT] sends the packet from another PC that is on the machine's network (the laptop is far away, and a magic packet cannot cross a router): this very file is piped to
`python3 -` over ssh on that PC, so nothing has to be installed there (the PC needs python3 and nothing else). Name that PC's address on the machine's network with --bind (a PC with several
networks, a VPN or docker bridges must not choose it by itself) and give --broadcast; the limited broadcast 255.255.255.255 is left out then, since on such a PC it may leave by any interface.
--remote-command CMD runs an installed helper there instead (CMD receives the MAC address as its argument), for a login that is only allowed to run that.
"""
import argparse
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def parse_mac(text):
    digits = re.sub(r"[^0-9a-fA-F]", "", text)
    if len(digits) != 12:
        raise ValueError(f"not a MAC address: {text!r} (twelve hex digits, with or without : or -)")
    return bytes.fromhex(digits)


def magic_packet(mac):
    mac = parse_mac(mac) if isinstance(mac, str) else mac
    return b"\xff" * 6 + mac * 16


def local_address_towards(host):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 9))
        return s.getsockname()[0]


def same_24(a, b):
    return a.split(".")[:3] == b.split(".")[:3]


def sources_towards(host):
    """The addresses of this PC to send from: those in the same /24 as `host` (every adapter's address that the system tells us about), else the route's choice."""
    found = []
    try:
        found = [a for a in socket.gethostbyname_ex(socket.gethostname())[2] if not a.startswith(("127.", "169.254."))]
    except OSError:
        pass
    mine = [a for a in dict.fromkeys(found) if same_24(a, host)]
    return mine or [local_address_towards(host)]


def wake_through(args):
    """Sends the packet from the jump host: this file on its standard input to python3 there (or the installed helper named by --remote-command). Returns the exit status."""
    if args.remote_command:
        command = shlex.join([args.remote_command, args.mac]) if hasattr(shlex, "join") else " ".join(shlex.quote(a) for a in [args.remote_command, args.mac])
        stdin = None
    else:
        inner = [args.remote_python, "-", args.mac, "--to", args.to, "--broadcast", args.broadcast, "--seconds", f"{args.seconds:g}", "--interval", f"{args.interval:g}",
                 "--no-limited-broadcast"] + (["--bind", args.bind] if args.bind else [])
        command = " ".join(shlex.quote(a) for a in inner)
        stdin = Path(__file__).read_bytes()
    sys.path.insert(0, str(Path(__file__).resolve().parent))      # (only here: the copy of this file that runs on the jump host has neither a __file__ nor tools/sshvia.py)
    import sshvia
    try:
        hop = sshvia.Hop(args.via, key=args.via_key, options=args.via_option)
    except ValueError as e:
        print(f"wol: {e}", file=sys.stderr)
        return 2
    exe = shutil.which(args.ssh) or args.ssh
    with hop:
        argv = [exe, *hop.ssh_args(), *hop.destination(), command]
        print(f"wol: asking {args.via} to send the magic packet for {args.mac}", flush=True)
        try:
            p = subprocess.Popen(argv, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as e:
            print(f"wol: cannot run {exe}: {e}", file=sys.stderr)
            return 1
        if stdin is not None:
            try:
                p.stdin.write(stdin)
                p.stdin.close()
            except OSError:
                pass                                            # ssh failed early: its message is the answer
        for raw in p.stdout:
            print("  " + raw.decode("utf-8", "replace").rstrip("\r\n"), flush=True)
        rc = p.wait()
    if rc != 0:
        print(f"wol: the jump host did not send the packet (status {rc}); ssh's message is above", file=sys.stderr)
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mac")
    ap.add_argument("--to", default="192.168.1.185", help="the machine's usual address: only used to find this PC's address on its network")
    ap.add_argument("--broadcast", default="192.168.1.255", help="the subnet's broadcast address (the limited broadcast 255.255.255.255 is always used too)")
    ap.add_argument("--bind", default=None, help="this PC's address to send from (default: found from --to)")
    ap.add_argument("--seconds", type=float, default=20.0, help="keep sending for this long (default 20)")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between bursts (default 2)")
    ap.add_argument("--no-limited-broadcast", action="store_true", help="send to the subnet's broadcast address only, not to 255.255.255.255")
    ap.add_argument("--via", default=None, metavar="[USER@]HOST[:PORT]", help="send the packet from this PC (over ssh) instead of this one")
    ap.add_argument("--via-key", default=None, metavar="FILE", help="the private key for --via (default: whatever ssh would use)")
    ap.add_argument("--via-option", action="append", default=[], metavar="OPT", help="an extra ssh option for --via, e.g. UserKnownHostsFile=FILE (repeatable)")
    ap.add_argument("--remote-python", default="python3", help="the python on the --via PC (default python3)")
    ap.add_argument("--remote-command", default=None, metavar="CMD", help="run this installed helper on the --via PC (it gets the MAC address as its argument) instead of piping this file")
    ap.add_argument("--ssh", default="ssh", help="the ssh client for --via (default: ssh from the PATH)")
    args = ap.parse_args(argv)
    try:
        packet = magic_packet(args.mac)
    except ValueError as e:
        print(f"wol: {e}", file=sys.stderr)
        return 2
    if args.via:
        return wake_through(args)
    sources = [args.bind] if args.bind else sources_towards(args.to)
    socks = []
    for src in sources:
        broadcasts = ([] if args.no_limited_broadcast else ["255.255.255.255"]) + ([args.broadcast] if same_24(src, args.broadcast) else [])
        if not broadcasts:
            print(f"wol: {src} and the broadcast address {args.broadcast} are not in the same /24: nothing to send to", file=sys.stderr)
            return 2
        targets = [(a, p) for a in broadcasts for p in (9, 7)]
        print(f"wol: {len(packet)}-byte magic packet for {args.mac}, from {src} to {', '.join(f'{a}:{p}' for a, p in targets)} for {args.seconds:g} s", flush=True)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind((src, 0))
        socks.append((sock, targets))
    end = time.time() + args.seconds
    sent = 0
    while True:
        for sock, targets in socks:
            for target in targets:
                sock.sendto(packet, target)
                sent += 1
        if time.time() + args.interval > end:
            break
        time.sleep(args.interval)
    print(f"wol: sent {sent} packets", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
