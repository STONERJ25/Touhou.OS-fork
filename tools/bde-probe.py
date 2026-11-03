#!/usr/bin/env python3
"""Read-only look at the BitLocker metadata of a Windows volume on a running TouhouOS machine, through ssh and `sudo dd` (it only reads).

  python tools/bde-probe.py [--part nvme0n1p3] [--host 192.168.1.185] [--known-hosts assets/local/dell_known_hosts]

A BitLocker volume's first sector carries the signature -FVE-FS- and the offsets of three metadata blocks. A key-protector entry there says how the volume master key is protected:
  0x0000 clear key        BitLocker is SUSPENDED (or was never activated): the key sits unprotected on the disk
  0x0100 TPM   0x0200 startup key   0x0500 TPM + PIN   0x0800 recovery password   0x2000 password
Written on 2026-10-25 before `install --alongside` touched the Dell's partition table: the volume had only a clear-key protector (Windows 11 "encrypted, waiting for activation"), so there was no recovery
key to lose and nothing for a partition-table change to trip. The layout follows the public description of the format (libbde's documentation).
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = {0x0000: "clear key (SUSPENDED or not yet activated)", 0x0100: "TPM", 0x0200: "startup key", 0x0500: "TPM + PIN", 0x0600: "TPM + startup key", 0x0800: "recovery password", 0x2000: "password"}


def protectors(block):
    """Protection types of the volume master key entries in one FVE metadata block (None if the block is not valid)."""
    if len(block) < 0x80 or block[:8] != b"-FVE-FS-":
        return None
    msize, _, hsize = struct.unpack_from("<III", block, 0x40)
    pos, end, found = 0x40 + hsize, min(0x40 + msize, len(block)), []
    while pos + 8 <= end:
        size, etype, vtype, _ = struct.unpack_from("<HHHH", block, pos)
        if size < 8:
            break
        if etype == 2 and vtype == 8 and size >= 8 + 28:
            found.append(struct.unpack_from("<H", block, pos + 8 + 26)[0])
        pos += size
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--part", default="nvme0n1p3", help="the volume's device node on the machine")
    ap.add_argument("--host", default="192.168.1.185")
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519")
    ap.add_argument("--known-hosts", default=str(ROOT / "assets" / "local" / "dell_known_hosts"))
    args = ap.parse_args(argv)
    base = ["ssh", "-i", str(Path(args.key).expanduser()), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", f"UserKnownHostsFile={Path(args.known_hosts).as_posix()}", "-o", "ConnectTimeout=10", "-q", f"{args.user}@{args.host}"]

    def read(offset, length):
        assert offset % 512 == 0 and length % 512 == 0
        return subprocess.run(base + [f"sudo dd if=/dev/{args.part} bs=512 skip={offset // 512} count={length // 512} status=none 2>/dev/null"], capture_output=True, timeout=180).stdout

    hdr = read(0, 512)
    if len(hdr) < 512 or hdr[3:11] != b"-FVE-FS-":
        print("this volume header is not BitLocker's (no -FVE-FS- signature at byte 3)")
        return 1
    offsets = struct.unpack_from("<QQQ", hdr, 0xB0)
    print("FVE metadata block offsets:", [hex(o) for o in offsets])
    for o in offsets:
        if not o or o % 512:
            continue
        found = protectors(read(o, 64 * 1024))
        if found is None:
            print(f"  block at {hex(o)}: no signature")
            continue
        print(f"  block at {hex(o)}: key protectors: " + (", ".join(f"{p:#06x} {NAMES.get(p, '?')}" for p in found) or "(none found)"))
        print("  suspended or not activated:", "YES (a clear-key protector is present)" if 0 in found else "no (protection is on)")
        print("  recovery password protector:", "YES" if 0x0800 in found else "NO (no numerical recovery password exists)")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
