#!/usr/bin/env python3
"""mkimage -- build a bootable TouhouOS disk image for a USB stick (or a virtual disk).

  python tools/mkimage.py                                   build everything, write build/touhou-os.img (1 GiB)
  python tools/mkimage.py --size 4G --out E:/touhou-os.img  bigger image, other place
  python tools/mkimage.py --cmdline "video=max login blk.ro tz=EST5EDT,M3.2.0,M11.1.0"
  python tools/mkimage.py --no-build                        use what build/ already holds
  python tools/mkimage.py --zip                             also write build/touhou-os.img.zip (flashable as it is)

The image is a GPT disk with a 64 MiB FAT32 EFI System Partition (EFI/BOOT/BOOTX64.EFI, kernel.elf, initrd.img,
cmdline.txt) and a FAT32 partition labelled HUMANVILLAGE for the rest, which is the only volume TouhouOS mounts by
itself. Write the image to a stick with Rufus (DD Image mode), balenaEtcher or dd: everything on the stick is replaced.
docs/REAL_HARDWARE.md walks through the whole thing for a PC.

The default kernel command line is "video=native blk.ro": the screen mode the firmware already chose (a monitor's own
resolution, normally) and every SATA/NVMe disk read-only, so a first boot on a real machine cannot change a byte of its
internal disks. Remove blk.ro when you want the OS to be allowed to write to a disk you mount yourself.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DEFAULT_CMDLINE = "video=native blk.ro"

README = """This is the HUMANVILLAGE volume of a TouhouOS stick: the homes of the residents live here
(reimu/, marisa/, sakuya/ ...), in ordinary FAT32 files that any computer can read.

TouhouOS mounts only the volume with this label and never touches any other disk.
Eject the stick safely before unplugging it, otherwise the volume is marked dirty and the next boot
mounts it read-only (see docs/STORAGE.md).
"""


def run(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(BUILD / "touhou-os.img"), help="image file to write (default build/touhou-os.img)")
    ap.add_argument("--size", default="1G", help="image size (default 1G; the HUMANVILLAGE partition takes what the ESP leaves)")
    ap.add_argument("--esp-size", default="64M")
    ap.add_argument("--cmdline", default=DEFAULT_CMDLINE, help=f"kernel command line, written to cmdline.txt (default '{DEFAULT_CMDLINE}')")
    ap.add_argument("--no-build", action="store_true", help="do not run tools/build.py first")
    ap.add_argument("--no-check", action="store_true", help="skip the fsck of the finished image")
    ap.add_argument("--zip", action="store_true", help="also write OUT.zip (a few MB: the image is mostly empty); Rufus and balenaEtcher flash a zipped image directly")
    args = ap.parse_args()

    if not args.no_build:
        print("[build]")
        run([sys.executable, ROOT / "tools" / "build.py"])

    esp = BUILD / "esp"
    needed = [esp / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "kernel.elf", esp / "initrd.img"]
    missing = [str(p) for p in needed if not p.exists()]
    if missing:
        sys.exit("mkimage: missing " + ", ".join(missing) + " (run tools/build.py)")

    # build/esp/cmdline.txt is rewritten by every test run, so the image gets a staged copy with its own command line
    print("[stage]")
    stage = BUILD / "image-esp"
    if stage.exists():
        shutil.rmtree(stage)
    for src in needed:
        dst = stage / src.relative_to(esp)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    (stage / "cmdline.txt").write_text(args.cmdline.strip() + "\n")
    hv = BUILD / "image-hv"
    if hv.exists():
        shutil.rmtree(hv)
    hv.mkdir(parents=True)
    (hv / "README.TXT").write_text(README.replace("\n", "\r\n"))
    print(f"  command line: {args.cmdline.strip()!r}")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    print("[image]")
    run([sys.executable, ROOT / "tools" / "mkdisk.py", out, "--size", args.size, "--esp", stage, "--esp-size", args.esp_size,
         "--hv-size", "rest", "--hv-dir", hv])
    if not args.no_check:
        print("[check]")
        run([sys.executable, ROOT / "tools" / "mkdisk.py", "--check", out])

    if args.zip:
        print("[zip]")
        zpath = out.with_name(out.name + ".zip")
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as z:
            z.write(out, out.name)
        print(f"  {zpath}  ({zpath.stat().st_size >> 10} KiB)")
    sha = hashlib.sha256()
    with open(out, "rb") as f:
        while chunk := f.read(1 << 22):
            sha.update(chunk)
    print()
    print(f"image  {out}  ({out.stat().st_size >> 20} MiB)")
    print(f"sha256 {sha.hexdigest()}")
    print()
    print("Write it to a USB stick with Rufus (DD Image mode), balenaEtcher, or dd; everything on the stick is replaced.")
    print("Then: Secure Boot off, boot the stick through the firmware's boot menu (UEFI entry). See docs/REAL_HARDWARE.md.")


if __name__ == "__main__":
    main()
