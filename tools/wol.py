#!/usr/bin/env python3
"""Sends a Wake-on-LAN "magic packet" to a machine that is switched off (docs/the project plan, track W).

  python tools/wol.py aa:bb:cc:dd:ee:ff                                 the Dell's network card (the packet goes out for 20 seconds)
  python tools/wol.py aa:bb:cc:dd:ee:ff --seconds 5 --broadcast 192.168.1.255 --bind 192.168.1.15

The magic packet is 6 bytes of FF followed by the machine's MAC address 16 times; the card's wake-up filter looks for that pattern in the payload of any frame, so a UDP broadcast does (port 9 "discard", also 7).
It is sent to the limited broadcast address and to the subnet's, from the dev PC's address on the machine's network: on a PC with several adapters (VPNs, Tailscale) the operating system would otherwise pick
the wrong one for a broadcast (a VPN with a default route sent the first packets of 2026-11-03 into the tunnel). --bind names the address to send from; by default the packets go out from **every** address of this PC that
lies in the same /24 as --to (found from the adapters' addresses, and from the route's choice as a last resort), each to its own subnet's broadcast too.
"""
import argparse
import re
import socket
import sys
import time


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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mac")
    ap.add_argument("--to", default="192.168.1.185", help="the machine's usual address: only used to find this PC's address on its network")
    ap.add_argument("--broadcast", default="192.168.1.255", help="the subnet's broadcast address (the limited broadcast 255.255.255.255 is always used too)")
    ap.add_argument("--bind", default=None, help="this PC's address to send from (default: found from --to)")
    ap.add_argument("--seconds", type=float, default=20.0, help="keep sending for this long (default 20)")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between bursts (default 2)")
    args = ap.parse_args(argv)
    packet = magic_packet(args.mac)
    sources = [args.bind] if args.bind else sources_towards(args.to)
    socks = []
    for src in sources:
        broadcasts = ["255.255.255.255"] + ([args.broadcast] if same_24(src, args.broadcast) else [])
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
