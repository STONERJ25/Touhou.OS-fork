#!/usr/bin/env python3
"""Prints a classic pcap file one line per frame: enough of Ethernet, ARP, IPv4, IPv6, ICMP, UDP and TCP to follow a conversation by eye.

  python tools/pcapdump.py FILE [--tcp PORT] [--abs] [--limit N]

Each line: the frame's number, the time in seconds since the first frame of the file (and, after "+", since the line printed before it), then
  ARP   who-has 10.0.2.2 tell 10.0.2.15            / is-at 52:55:0a:00:02:02
  TCP   10.0.2.2:51234 > 10.0.2.15:22 [S.AP FR] seq=... ack=... win=... len=N  (seq and ack relative to each direction's first number unless --abs)
  UDP   10.0.2.15:68 > 255.255.255.255:67 len=N
  ICMP  10.0.2.15 > 10.0.2.2 type 8 code 0

--tcp PORT keeps the TCP segments with that source or destination port, and every ARP frame (an address resolution in the way of a segment is what one looks for).
The captures come from the test harness: TOUHOU_PCAP=build/x.pcap (tests/harness.py), written by QEMU's filter-dump, which sees the frames between the guest's
card and QEMU's user-mode network. Standard library only. The formats: the pcap file header and record header (magic a1b2c3d4, microseconds, or a1b23c4d,
nanoseconds; link type 1 = Ethernet), RFC 826 (ARP), RFC 791/792 (IPv4, ICMP), RFC 8200/4443 (IPv6, ICMPv6), RFC 768 (UDP), RFC 9293 (TCP).
"""
import argparse
import struct
import sys


def frames(path):
    """Yields (seconds, bytes) for every record of a classic pcap file."""
    with open(path, "rb") as f:
        head = f.read(24)
        if len(head) < 24:
            return
        magic = head[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            end = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            end = ">"
        else:
            raise SystemExit(f"{path}: not a classic pcap file (magic {magic.hex()})")
        frac = 1e-9 if magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d") else 1e-6
        link = struct.unpack(end + "I", head[20:24])[0]
        if link != 1:
            raise SystemExit(f"{path}: link type {link}, only Ethernet (1) is read")
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            sec, sub, caplen, _ = struct.unpack(end + "IIII", rec)
            data = f.read(caplen)
            if len(data) < caplen:
                return                                              # a capture that was cut while QEMU wrote it
            yield sec + sub * frac, data


def mac(b):
    return ":".join(f"{x:02x}" for x in b)


def ip4(b):
    return ".".join(str(x) for x in b)


def ip6(b):
    words = [f"{(b[i] << 8) | b[i + 1]:x}" for i in range(0, 16, 2)]
    return ":".join(words)                                          # (not compressed: unambiguous is enough here)


class Tcp:
    """Relative sequence numbers: the first sequence number seen from each endpoint of a connection is its zero."""

    def __init__(self, absolute):
        self.absolute, self.base = absolute, {}

    def line(self, src, dst, seg):
        if len(seg) < 20:
            return None, f"TCP   {src} > {dst} (truncated)"
        sport, dport, seq, ack, off, flags, win = struct.unpack(">HHIIBBH", seg[:16])
        payload = len(seg) - (off >> 4) * 4
        a, b = f"{src}:{sport}", f"{dst}:{dport}"
        if not self.absolute:
            if flags & 0x02 or (a, b) not in self.base:
                self.base[(a, b)] = seq
            seq = (seq - self.base[(a, b)]) & 0xFFFFFFFF
            if flags & 0x10 and (b, a) in self.base:
                ack = (ack - self.base[(b, a)]) & 0xFFFFFFFF
        names = "".join(ch if flags & bit else "" for bit, ch in ((0x02, "S"), (0x10, "."), (0x08, "P"), (0x01, "F"), (0x04, "R"), (0x20, "U")))
        text = f"TCP   {a} > {b} [{names}] seq={seq}" + (f" ack={ack}" if flags & 0x10 else "") + f" win={win} len={payload}"
        return (sport, dport), text


def describe(data, tcp):
    """(kind, ports or None, text) of one Ethernet frame."""
    if len(data) < 14:
        return "short", None, f"short frame ({len(data)} bytes)"
    etype = struct.unpack(">H", data[12:14])[0]
    p = data[14:]
    if etype == 0x0806 and len(p) >= 28:
        op = struct.unpack(">H", p[6:8])[0]
        sha, spa, tpa = p[8:14], p[14:18], p[24:28]
        if op == 1:
            return "arp", None, f"ARP   who-has {ip4(tpa)} tell {ip4(spa)} ({mac(sha)})"
        return "arp", None, f"ARP   {ip4(spa)} is-at {mac(sha)}" + ("" if op == 2 else f" (op {op})")
    if etype == 0x0800 and len(p) >= 20:
        ihl = (p[0] & 15) * 4
        total = struct.unpack(">H", p[2:4])[0]
        proto, src, dst = p[9], ip4(p[12:16]), ip4(p[16:20])
        body = p[ihl:total] if ihl <= total <= len(p) else p[ihl:]
        frag = struct.unpack(">H", p[6:8])[0] & 0x3FFF
        if frag and proto in (6, 17):
            return "ip", None, f"IPv4  {src} > {dst} proto {proto} fragment (offset/MF {frag:#x}) len={len(body)}"
    elif etype == 0x86DD and len(p) >= 40:
        plen = struct.unpack(">H", p[4:6])[0]
        proto, src, dst = p[6], ip6(p[8:24]), ip6(p[24:40])
        body = p[40:40 + plen]
        src, dst = f"[{src}]", f"[{dst}]"
    else:
        return "other", None, f"ether type {etype:#06x} {mac(data[6:12])} > {mac(data[0:6])} len={len(p)}"
    if proto == 6:
        ports, text = tcp.line(src, dst, body)
        return "tcp", ports, text
    if proto == 17 and len(body) >= 8:
        sport, dport, ulen = struct.unpack(">HHH", body[:6])
        return "udp", (sport, dport), f"UDP   {src}:{sport} > {dst}:{dport} len={ulen - 8}"
    if proto in (1, 58) and len(body) >= 2:
        return "icmp", None, f"{'ICMP ' if proto == 1 else 'ICMP6'} {src} > {dst} type {body[0]} code {body[1]}"
    return "ip", None, f"IP    {src} > {dst} proto {proto} len={len(body)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--tcp", type=int, metavar="PORT", help="only the TCP segments from or to this port (and every ARP frame)")
    ap.add_argument("--abs", action="store_true", help="absolute sequence and acknowledgement numbers")
    ap.add_argument("--limit", type=int, default=0, help="stop after this many printed lines")
    args = ap.parse_args()
    tcp = Tcp(args.abs)
    first = last = None
    shown = 0
    for n, (t, data) in enumerate(frames(args.file), 1):
        if first is None:
            first = t
        kind, ports, text = describe(data, tcp)
        if args.tcp is not None and not (kind == "arp" or (kind == "tcp" and ports and args.tcp in ports)):
            continue
        gap = 0.0 if last is None else t - last
        last = t
        print(f"{n:5d} {t - first:10.6f} +{gap:9.6f}  {text}")
        shown += 1
        if args.limit and shown >= args.limit:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
