#!/usr/bin/env python3
"""Independent, read-only check of a disk's GUID partition table on a running TouhouOS machine, through ssh and `sudo dd` (it only reads).

  python tools/gpt-check.py [--disk nvme0n1] [--host 192.168.1.185] [--known-hosts assets/local/dell_known_hosts]

Reads both copies of the GPT header and entry array, checks their checksums and that the copies agree, and prints the entries (type GUID, first and last sector, size, name). Written after
`install --alongside` ran on the Dell (2026-10-25): an implementation that is not the installer's own, so that "Windows' partitions are unchanged" is not the installer's word alone.
`--expect 1:2048-1026047,2:1026048-1288191` additionally requires those entries (number:first-last sector) to be exactly where they are given.
"""
import argparse
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SS = 512


def make_reader(args):
    base = ["ssh", "-i", str(Path(args.key).expanduser()), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", f"UserKnownHostsFile={Path(args.known_hosts).as_posix()}", "-o", "ConnectTimeout=10", "-q", f"{args.user}@{args.host}"]

    def read(lba, sectors):
        out = subprocess.run(base + [f"sudo dd if=/dev/{args.disk} bs=512 skip={lba} count={sectors} status=none 2>/dev/null"], capture_output=True, timeout=180).stdout
        if len(out) != sectors * SS:
            raise SystemExit(f"short read at LBA {lba}: {len(out)} of {sectors * SS} bytes (is the machine up, and the key pinned?)")
        return out

    return read


def parse_header(h, tag):
    if h[:8] != b"EFI PART":
        raise SystemExit(f"{tag}: no GPT signature")
    hsize, crc = struct.unpack_from("<II", h, 12)[0], struct.unpack_from("<I", h, 16)[0]
    tmp = bytearray(h[:hsize])
    tmp[16:20] = b"\0\0\0\0"
    if zlib.crc32(bytes(tmp)) & 0xFFFFFFFF != crc:
        raise SystemExit(f"{tag}: header checksum wrong")
    my, alt, first, last = struct.unpack_from("<QQQQ", h, 24)
    ent_lba, nent, esize, ecrc = struct.unpack_from("<QIII", h, 72)
    return dict(my=my, alt=alt, first=first, last=last, guid=uuid.UUID(bytes_le=bytes(h[56:72])), ent_lba=ent_lba, nent=nent, esize=esize, ecrc=ecrc)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default="nvme0n1")
    ap.add_argument("--host", default="192.168.1.185")
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519")
    ap.add_argument("--known-hosts", default=str(ROOT / "assets" / "local" / "dell_known_hosts"))
    ap.add_argument("--expect", default="", help="entries that must be exactly here: NUMBER:FIRST-LAST,...")
    args = ap.parse_args(argv)
    read = make_reader(args)
    prim = parse_header(read(1, 1)[:SS], "primary")
    back = parse_header(read(prim["alt"], 1)[:SS], "backup")
    print(f"disk: {prim['alt'] + 1} sectors ({(prim['alt'] + 1) * SS / 2**30:.1f} GiB), disk GUID {prim['guid']}; usable {prim['first']}..{prim['last']}")
    for tag, hd in (("primary", prim), ("backup", back)):
        sectors = (hd["nent"] * hd["esize"] + SS - 1) // SS
        arr = read(hd["ent_lba"], sectors)[:hd["nent"] * hd["esize"]]
        if zlib.crc32(arr) & 0xFFFFFFFF != hd["ecrc"]:
            raise SystemExit(f"{tag}: entry array checksum wrong")
        hd["arr"] = arr
        print(f"{tag}: header and entry array checksums OK ({hd['nent']} entries of {hd['esize']} bytes at LBA {hd['ent_lba']})")
    if prim["arr"] != back["arr"] or prim["guid"] != back["guid"]:
        raise SystemExit("the two copies of the table differ")
    print("both copies agree")
    rows = {}
    for i in range(prim["nent"]):
        e = prim["arr"][i * prim["esize"]:(i + 1) * prim["esize"]]
        if not any(e[:16]):
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        rows[i + 1] = (first, last)
        print(f"  entry {i + 1}: type {uuid.UUID(bytes_le=bytes(e[:16]))}  LBA {first}..{last} ({(last - first + 1) * SS / 2**20:.0f} MiB)  '{e[56:128].decode('utf-16-le').rstrip(chr(0))}'")
    ok = True
    for item in filter(None, args.expect.split(",")):
        n, span = item.split(":")
        f, l = (int(x) for x in span.split("-"))
        same = rows.get(int(n)) == (f, l)
        print(f"  entry {n}: {'as expected' if same else 'DIFFERENT OR MISSING'}")
        ok &= same
    print("RESULT:", "all as expected" if ok else "SOMETHING IS DIFFERENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
