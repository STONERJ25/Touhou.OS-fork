"""The door to the shrine: the graphical greeter and the text-mode login.

The guest clock slows down whenever the host is busy, so nothing here sleeps for a fixed time and then expects an
effect: the tests knock again until the serial console shows what they are waiting for."""
import time

import test_shell
from harness import test, Qemu, BUILD


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


def wait_for_pixel(vm, x, y, want, timeout=40, tol=50):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if near(vm.pixel(x, y, vm.screen_ppm()), want, tol):
            return True
        time.sleep(0.5)
    return False


def retry_until(vm, action, pattern, tries=20, wait=3):
    """Performs `action` (typing something), waiting `wait` seconds for `pattern` on the serial console; repeats."""
    for _ in range(tries):
        action()
        try:
            return vm.expect(pattern, timeout=wait)
        except Exception:  # noqa: BLE001 -- the harness raises on timeout; try again
            if not vm.alive():
                raise
    raise AssertionError(f"never saw /{pattern}/; serial tail:\n{vm.text()[-600:]}")


CARD_STRIPE = (512, 190, (0xC8, 0x10, 0x2E))           # the shrine-red band on top of the login card
START_BUTTON = (118, 756, (0xB5, 0x12, 0x2B))          # the desktop panel's start button


@test
def login_screen():
    """'login' shows the greeter; a wrong password is turned away, an empty one lets reimu in to her desktop."""
    with Qemu(cmdline="login", name="t-login-screen", mem="768M") as vm:
        vm.expect(r"opening the login screen", timeout=90)
        assert wait_for_pixel(vm, *CARD_STRIPE), "the login card never appeared"
        vm.screenshot(BUILD / "t-login-screen.png")
        time.sleep(1)
        vm.type("wrong\n")
        vm.expect(r"reimu was turned away at the gate", timeout=60)
        retry_until(vm, lambda: vm.type("\n"), r"opening the desktop for reimu")    # reimu has no password; the gate reopens soon
        assert wait_for_pixel(vm, *START_BUTTON), "the desktop panel never appeared after logging in"


@test
def login_switch_user():
    """Tab picks the next resident on the login screen; marisa gets her own desktop and her own uid."""
    with Qemu(cmdline="login", name="t-login-marisa", mem="768M") as vm:
        vm.expect(r"opening the login screen", timeout=90)
        assert wait_for_pixel(vm, *CARD_STRIPE), "the login card never appeared"
        time.sleep(1)
        vm.key("tab")
        time.sleep(1)
        vm.type("\n")                                                     # marisa has no password either
        vm.expect(r"opening the desktop for marisa", timeout=60)
        assert wait_for_pixel(vm, *START_BUTTON), "the desktop panel never appeared for marisa"
        time.sleep(3)
        vm.key("t", ctrl=True, alt=True)
        time.sleep(5)
        retry_until(vm, lambda: vm.type("logger -t Terminal who-is-$(whoami)\n"), r"who-is-marisa", tries=8, wait=5)


@test
def login_text():
    """'text login' asks for a name and a password on the console; unknown names are refused, residents get their shell."""
    with Qemu(cmdline="text login", name="t-login-text") as vm:
        vm.expect(r"gensokyo login: ", timeout=90)
        vm.send("nobody\n")
        vm.expect(r"Password: ", timeout=15)
        vm.send("\n")
        vm.expect(r"Login incorrect", timeout=30)
        vm.expect(r"gensokyo login: ", timeout=15)
        vm.send("marisa\n")
        vm.expect(r"Password: ", timeout=15)
        vm.send("\n")
        vm.expect(r"marisa@gensokyo \S+ \$ ", timeout=30)
        out, _ = test_shell.run(vm, "whoami; id -u", prompt=r"marisa@gensokyo \S+ \$ ")
        assert "marisa\n1001" in out, out
        vm.send("exit\n")
        vm.expect(r"gensokyo login: ", timeout=30)                       # a shell that ends brings the door back


@test
def autologin_option():
    """'text autologin=sakuya' opens sakuya's shell straight away."""
    with Qemu(cmdline="text autologin=sakuya", name="t-autologin") as vm:
        vm.expect(r"sakuya@gensokyo \S+ \$ ", timeout=90)
        vm.send("whoami\n")
        vm.expect(r"\nsakuya\n", timeout=15)
