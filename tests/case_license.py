"""Every image carries the license and the notices (MIT asks that the notice stays with all copies; docs/LICENSING.md).

    python tools/qemu-slot.py -- python tests/run_all.py license
"""
from harness import test, Qemu

DOC = "/ScarletDevilMansion/share/doc/touhouos"


@test
def license_travels_with_the_image():
    """LICENSE, THIRD_PARTY_NOTICES.md and the DejaVu license are in the initrd, in /ScarletDevilMansion/share/doc/touhouos"""
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
