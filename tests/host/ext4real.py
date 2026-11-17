#!/usr/bin/env python3
"""A volume that a real Linux wrote, read by the parser and compared with what that Linux says about the mounted tree (docs/EXT4.md, "Checked against a volume that a real Linux wrote").

  wsl.exe -u root -e python3 tests/host/ext4real.py /dev/sdd /usr          (the root file system of WSL's Ubuntu, read raw and read-only; needs build/host-ext4/ext4tool, made by test_ext4.py)
  sudo python3 tests/host/ext4real.py /dev/sdb1 /media/me/STICK          (a stick: the device and where Linux has mounted it)

`ext4tool DEVICE walk TOP` walks the volume with the kernel's parser (every file read, every name looked up again) and this script walks the same directory of the mounted tree with the
operating system's own calls and compares, for every entry: kind, mode, owner, inode number, link count, size, time of modification, the crc32 of the content (files up to 64 MiB) and the
target of a link; and that the parser lists nothing the mounted tree does not have. Nothing is written. Two things make an honest difference: files that were being written (the disk lags
the cache, so a log or a clock file differs in size or time), and paths below another mount (the parser reads one volume, `os.walk` crosses mounts: /usr/lib/modules of WSL is an overlay).
"""
import os
import stat
import subprocess
import sys
import zlib
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent.parent / "build" / "host-ext4" / "ext4tool"
KINDS = {stat.S_IFREG: "f", stat.S_IFDIR: "d", stat.S_IFLNK: "l", stat.S_IFCHR: "c", stat.S_IFBLK: "b", stat.S_IFIFO: "p", stat.S_IFSOCK: "s"}


def crc_of(path):
    c = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            c = zlib.crc32(chunk, c)
    return c & 0xFFFFFFFF


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    dev, top = argv
    if not TOOL.exists():
        print(f"{TOOL} does not exist: run `python tools/hosttest.py discover -s tests/host -p test_ext4.py -k Source` once to build it")
        return 2
    info = subprocess.run([str(TOOL), dev, "info"], capture_output=True, text=True).stdout
    keep = ("flavour", "block_size", "blocks", "inodes", "groups", "features")
    print("\n".join(line for line in info.splitlines() if line.split("=")[0] in keep))
    r = subprocess.run([str(TOOL), dev, "walk", top], capture_output=True)
    rows = {}
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        p = line.split("\t")
        if len(p) >= 12:
            rows[p[1]] = dict(kind=p[0], size=int(p[2]), mode=int(p[3], 8), uid=int(p[4]), gid=int(p[5]), nlink=int(p[6]), ino=int(p[7]), mtime=int(p[8]), tail="\t".join(p[11:]))
        elif p[0] == "E":
            rows[p[1]] = dict(kind="E", err=p[2])
    print(f"the parser's walk of {top}: exit {r.returncode}, {len(rows)} entries")
    diffs, same, big, kinds = [], 0, 0, {}
    for dirpath, dirnames, filenames in os.walk(top):
        for name in dirnames + filenames:
            p = os.path.join(dirpath, name)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            row = rows.get(p)
            if row is None:
                diffs.append((p, "not in the parser's walk (another mount, or new since the volume was last written)"))
                continue
            k = KINDS[stat.S_IFMT(st.st_mode)]
            if row["kind"] == "E":
                diffs.append((p, "the parser says error " + row["err"]))
            elif row["kind"] != k:
                diffs.append((p, f"kind {row['kind']} against {k}"))
            elif (row["mode"], row["uid"], row["gid"]) != (st.st_mode & 0o7777, st.st_uid, st.st_gid):
                diffs.append((p, "mode or owner"))
            elif row["ino"] != st.st_ino or (k != "d" and row["nlink"] != st.st_nlink):
                diffs.append((p, f"inode {row['ino']}/{st.st_ino} or links {row['nlink']}/{st.st_nlink}"))
            elif k == "f" and (row["size"] != st.st_size or row["mtime"] != int(st.st_mtime)):
                diffs.append((p, f"size {row['size']}/{st.st_size} or mtime {row['mtime']}/{int(st.st_mtime)}"))
            elif k == "f" and st.st_size <= 64 << 20 and row["tail"] != "%08x" % crc_of(p):
                diffs.append((p, "content"))
            elif k == "l" and row["tail"] != os.readlink(p):
                diffs.append((p, "link target"))
            else:
                same += 1
                kinds[k] = kinds.get(k, 0) + 1
                big += k == "f" and st.st_size > 64 << 20
    extra = [p for p, row in rows.items() if row["kind"] != "E" and not os.path.lexists(p)]
    print(f"{same} entries agree {kinds} ({big} files over 64 MiB compared by size and time only); {len(diffs)} differ; {len(extra)} entries only in the parser's walk")
    for d in diffs[:20]:
        print("  differs:", *d)
    for e in extra[:5]:
        print("  only in the walk:", e)
    return 0 if not extra and r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
