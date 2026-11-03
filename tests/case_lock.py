"""The screen lock: Super+L covers the desktop, shortcuts stop working, the right password (or none) lets go."""
import time

from guitools import Gui, START_BUTTON, SHRINE_RED, near
from harness import test, Qemu, scaled

LOCK_CARD = (640, 400)          # inside the lock card's avatar disc at 1024x768 (the lock window is full screen)


def lock_visible(g):
    """The lock darkens everything: the panel's red start button is gone and the moon is dim."""
    return not g.pixel_near(*START_BUTTON, SHRINE_RED, tol=60)


@test
def lock_screen():
    """Super+L locks: the panel disappears, Ctrl+Alt+T no longer opens a terminal, a wrong password is refused, Enter unlocks."""
    with Qemu(cmdline="text", name="t-lock", mem="768M") as vm:
        g = Gui(vm)
        g.start_desktop()
        # give reimu a password so the lock actually has something to check
        g.sh("echo '== setting a password'")
        vm.send("passwd\n")
        vm.expect(r"New password: ", timeout=20)
        vm.send("shrine\n")
        vm.expect(r"Retype new password: ", timeout=10)
        vm.send("shrine\n")
        vm.expect(r"password updated for reimu", timeout=20)
        vm.expect(r"reimu@gensokyo \S+ \$ ", timeout=10)

        time.sleep(1)
        vm.key("l", ctrl=False)                  # make sure nothing odd is focused before the shortcut
        g.shot("before-lock")
        # Super+L: hold the left Super key (the QMP name is 'meta_l')
        vm._input([vm._key_event("meta_l", True), vm._key_event("l", True)], vm.input_device)
        time.sleep(0.2)
        vm._input([vm._key_event("l", False), vm._key_event("meta_l", False)], vm.input_device)
        deadline = time.time() + scaled(20)
        while time.time() < deadline and not lock_visible(g):
            time.sleep(0.5)
        assert lock_visible(g), "the lock screen never appeared"
        g.shot("locked")

        # shortcuts are dead while locked: Ctrl+Alt+T must not start a terminal
        vm.key("t", ctrl=True, alt=True)
        time.sleep(3)
        assert lock_visible(g), "the lock should still be up"
        out = g.sh("ps")
        assert "gapterm" not in out, out

        # wrong password: still locked
        g.type("not the password\n")
        time.sleep(3)
        assert lock_visible(g), "a wrong password must not unlock"
        vm.expect(r"reimu failed to unlock the screen", timeout=60)

        # the right one lets go and the panel is back. The gate stays shut for a moment after a refusal (guest time, which
        # runs slow on a busy host), so type the password once and press Enter until it opens.
        g.type("shrine")
        unlocked = False
        for _ in range(20):
            g.key("ret")
            if g.wait_pixel(*START_BUTTON, SHRINE_RED, tol=60, timeout=4):
                unlocked = True
                break
        assert unlocked, "the right password did not unlock"
        g.shot("unlocked")
