"""Bit-exact screenshot comparison of two builds at 1024x768 (how the hi-dpi work checked that scale 1 is unchanged, docs/DESKTOP.md).

    git worktree add --detach /tmp/ref origin/main && (cd /tmp/ref && python3 tools/build.py)
    python3 tests/pixel_identity.py shots /tmp/ref build/ident-ref          # boots the build of that checkout
    python3 tests/pixel_identity.py shots .        build/ident-branch
    python3 tests/pixel_identity.py compare build/ident-ref build/ident-branch
    git worktree remove --force /tmp/ref

`shots` boots the checkout's build (one VM; never while a test run uses the same checkout), starts the desktop by hand and visits
the desktop, the start menu, a terminal and seven applications. Before each scene the clock is set to 12:00:00 (the panel shows
the minutes) and the pointer parked in the same place; then six frames are grabbed about half a second apart and the distinct
ones kept (a blinking caret, or a grab in the middle of a panel repaint, gives a second or third frame). `compare` passes when every
scene has at least one frame that is the same, byte for byte, in both runs, and prints where the other frames differ.

Two figures are data, not drawing, and differ by 0.1 MB between builds of different size: the memory in use on the Control
Panel's System page and the free space in Explorer's status bar. One digit cell of each is blanked before comparing.
"""
import hashlib
import sys
import time
from pathlib import Path

FIXED = "2026-10-24 12:00:00"
PARK = (1000, 400)          # the pointer, parked over the wallpaper's right edge
MASKS = {"controlpanel": (528, 392, 545, 409), "explorer": (752, 668, 769, 685)}


def shots(checkout, out):
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(checkout / "tests"))
    from harness import Qemu, scaled
    from guitools import Gui

    def set_clock(g):
        g.sh(f"su marisa -c \"sudo date -u -s '{FIXED}'\" > /dev/null")

    def capture(g, vm, name, frames=6, gap=0.45):
        set_clock(g)
        time.sleep(scaled(2.5))
        hashes = []
        for _ in range(frames):
            w, h, rgb = vm.screen_ppm()
            d = hashlib.sha256(rgb).hexdigest()[:16]
            if d not in hashes:
                hashes.append(d)
                (out / f"{name}-{d}.rgb").write_bytes(rgb)
                if len(hashes) == 1:
                    vm.screenshot(out / f"{name}.png")
            time.sleep(gap)
        (out / f"{name}.txt").write_text(f"{w}x{h}\n" + "\n".join(hashes) + "\n")
        print(name, w, h, hashes, flush=True)

    def app(g, vm, name, cmd, wait=8):
        g.sh(f"{cmd} > /tmp/{name}.log 2>&1 &")
        time.sleep(scaled(wait))
        g.move_to(*PARK)
        capture(g, vm, name)
        g.key("f4", alt=True)
        time.sleep(scaled(2))

    with Qemu(cmdline="text", name="t-pixel-identity", mem="768M") as vm:
        g = Gui(vm)
        g.wait_prompt()
        set_clock(g)
        g.sh("mkdir -p /tmp/hx; echo 'Reimu keeps the shrine.' > /tmp/hx/notes.txt; echo 'a,b' > /tmp/hx/list.csv; mkdir /tmp/hx/folder")
        g.start_desktop()
        time.sleep(scaled(3))
        g.move_to(*PARK)
        capture(g, vm, "desktop")
        g.click(118, 756)                                   # the start menu
        time.sleep(scaled(3))
        capture(g, vm, "menu")
        g.click(118, 756)
        time.sleep(scaled(2))
        g.move_to(*PARK)
        g.sh("gapterm > /tmp/gapterm.log 2>&1 &")
        time.sleep(scaled(8))
        g.type("echo hakurei $((6*7))\n")
        time.sleep(scaled(2))
        capture(g, vm, "terminal")
        g.key("f4", alt=True)
        time.sleep(scaled(2))
        app(g, vm, "calculator", "calculator")
        app(g, vm, "notebook", "notebook /tmp/hx/notes.txt")
        app(g, vm, "explorer", "explorer /tmp/hx")
        app(g, vm, "minesweeper", "minesweeper --seed 3")
        app(g, vm, "sketchbook", "sketchbook")
        app(g, vm, "controlpanel", "kappa-panel")
        app(g, vm, "kdemo", "kdemo")


def compare(ref, br):
    import numpy as np

    def frames(d, scene):
        lines = (d / f"{scene}.txt").read_text().split()
        w, h = map(int, lines[0].split("x"))
        res = {}
        for hsh in lines[1:]:
            a = np.frombuffer((d / f"{scene}-{hsh}.rgb").read_bytes(), np.uint8).reshape(h, w, 3).copy()
            if scene in MASKS:
                x0, y0, x1, y1 = MASKS[scene]
                a[y0:y1, x0:x1] = 0
            res[hashlib.sha256(a.tobytes()).hexdigest()[:16]] = a
        return res

    def bbox(a, b):
        m = (a != b).any(axis=2)
        ys, xs = np.nonzero(m)
        return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()), int(m.sum())) if len(xs) else (0, 0, 0, 0, 0)

    ok = True
    for t in sorted(ref.glob("*.txt")):
        scene = t.stem
        R, B = frames(ref, scene), frames(br, scene)
        common = set(R) & set(B)
        print(f"{scene}: {len(R)} reference frames, {len(B)} compared frames, identical in both: {len(common)}")
        ok = ok and bool(common)
        for name, A, O in (("compared", B, R), ("reference", R, B)):
            for hsh, a in A.items():
                if hsh not in O:
                    x0, y0, x1, y1, n = min((bbox(a, o) for o in O.values()), key=lambda x: x[4])
                    print(f"   {name} frame {hsh}: the nearest frame of the other run differs in x {x0}-{x1}, y {y0}-{y1} ({n} pixels)")
    print("every scene has a bit-exact match" if ok else "SOME SCENES HAVE NO BIT-EXACT MATCH")
    return ok


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "shots":
        shots(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
    elif len(sys.argv) == 4 and sys.argv[1] == "compare":
        sys.exit(0 if compare(Path(sys.argv[2]), Path(sys.argv[3])) else 1)
    else:
        sys.exit(__doc__)
