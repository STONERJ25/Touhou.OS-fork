"""Every image carries the license and the notices (MIT asks that the notice stays with all copies; docs/LICENSING.md).

    python tools/qemu-slot.py -- python tests/run_all.py license
"""
from pathlib import Path

from harness import test, Qemu

DOC = "/ScarletDevilMansion/share/doc/touhouos"
HAVE_FONTS = (Path(__file__).resolve().parents[1] / "assets" / "fonts" / "OFL.txt").is_file()


@test
def license_travels_with_the_image():
    """LICENSE, THIRD_PARTY_NOTICES.md, the DejaVu license and the Open Font License of the Gensokyo typefaces are in the initrd, in /ScarletDevilMansion/share/doc/touhouos"""
    with Qemu(cmdline="init=kshell", name="t-license") as vm:
        vm.expect(r"kshell:# ", timeout=60)
        vm.send(f"cat {DOC}/LICENSE\n")
        vm.expect(r"MIT License", timeout=10)
        vm.expect(r"Permission is hereby granted")
        vm.expect(r"kshell:# ")
        vm.send(f"cat {DOC}/THIRD_PARTY_NOTICES.md\n")
        vm.expect(r"Third-party notices", timeout=10)
        vm.expect(r"DejaVu Sans Mono")
        vm.expect(r"kshell:# ")
        vm.send(f"cat {DOC}/LICENSE_DEJAVU\n")
        vm.expect(r"Bitstream Vera Fonts Copyright", timeout=10)
        vm.expect(r"kshell:# ")
        if HAVE_FONTS:                                         # (the public export of this tree has no typefaces: nothing to carry then)
            vm.send(f"cat {DOC}/OFL.txt\n")                    # the fonts are in /ScarletDevilMansion/share/fonts: this is the license that asks to travel with them
            vm.expect(r"Copyright \(c\) 2026 STONERJ25", timeout=10)
            vm.expect(r"SIL OPEN FONT LICENSE Version 1\.1 - 26 February 2007")
            vm.expect(r"THE FONT SOFTWARE IS PROVIDED")
            vm.expect(r"kshell:# ")
            vm.send("ls /ScarletDevilMansion/share/fonts\n")
            vm.expect(r"GensokyoSans-Regular\.ttf", timeout=10)
            vm.expect(r"kshell:# ")
