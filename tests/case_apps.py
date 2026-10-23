"""The desktop applications: Ran's Calculator, Sakuya's Pocket Watch, the Spell Manager, the Kappa Control Panel and
Bunbunmaru News. The engines and data are checked through their command-line modes in a text-mode boot; the windows are
driven with the keyboard in a desktop boot and judged by the --trace lines they print on the serial console (the machine
under test can be slow, so every wait is on an event, never on a fixed delay) and by pixels on the screen."""
import re
import time

from harness import test, Qemu, BUILD
from test_shell import run, PROMPT


def lines(out):
    return [l.strip() for l in out.strip().splitlines() if l.strip()]


# ---------------------------------------------------------------- command-line modes

CALC = [
    ("2+3*4", "14"), ("(2+3)*4", "20"), ("2^3^2", "512"), ("-2^2", "-4"), ("2^-2", "0.25"), ("10/4", "2.5"), ("1/3", "0.333333333333333"),
    ("0.1+0.2", "0.3"), ("1/0", "error: Division by zero"), ("5!", "120"), ("200+10%", "220"), ("200-10%", "180"), ("200*10%", "20"),
    ("50%", "0.5"), ("-7 mod 3", "2"), ("2pi", "6.28318530717959"), ("sqrt(2)^2", "2"), ("sqrt 16", "4"), ("cbrt(27)", "3"), ("cbrt(-8)", "-2"),
    ("sin(30)", "0.5"), ("cos(60)", "0.5"), ("tan(45)", "1"), ("sin(180)", "0"), ("cos(90)", "0"), ("asin(0.5)", "30"), ("atan(1)", "45"),
    ("ln(e)", "1"), ("log(1000)", "3"), ("log2(8)", "3"), ("exp(0)", "1"), ("pow(2,10)", "1024"), ("abs(-5)", "5"), ("2^100", "1.26765060022823e30"),
    ("170!", "7.257415615308e306"), ("1755!", "error: Overflow: that factorial is too large"), ("1e15", "1e15"), ("1.5e-7", "1.5e-7"),
    ("123456789012345", "123456789012345"), ("1234567890123456", "1.23456789012346e15"), ("0.000001", "0.000001"), ("2e3", "2000"),
    ("3(4)", "12"), ("(1+2)(3+4)", "21"), ("round(-2.5)", "-3"), ("floor(-2.5)", "-3"), ("min(3,4)", "3"), ("mod(10,3)", "1"),
    ("sqrt(-1)", "error: The square root of a negative number is not real"), ("ln(0)", "error: ln needs a number above zero"),
    ("(2+3", "error: A closing parenthesis ')' is missing"), ("2+", "error: The expression ends too early: a number is missing"),
    ("2 3", "error: Two numbers in a row: an operator like + or * is missing"), ("foo(3)", "error: I do not know the name 'foo'"),
    ("tan(90)", "error: tan is undefined at this angle"), ("0^0", "1"), ("(-8)^(1/3)", "error: A negative number cannot be raised to a fractional power"),
]


@test
def apps_cli():
    """calculator, pocketwatch, spellmgr, kappa-panel and bunbunmaru command-line modes give the right answers."""
    with Qemu(cmdline="text", name="t-apps-cli", mem="768M") as vm:
        vm.expect(PROMPT, timeout=90)

        # Ran's Calculator: the expression engine
        args = " ".join("'" + e + "'" for e, _ in CALC)
        out, rc = run(vm, f"calculator --eval {args}", timeout=90)
        got = lines(out)
        assert len(got) == len(CALC), f"expected {len(CALC)} lines, got {len(got)}:\n{out}"
        bad = [f"{e!r}: expected {want!r}, got {g!r}" for (e, want), g in zip(CALC, got) if g != want]
        assert not bad, "calculator disagrees:\n" + "\n".join(bad)
        out, _ = run(vm, "calculator --eval --rad 'sin(pi)' 'cos(pi/2)' 'tan(pi/4)' 'ans+1'")
        assert lines(out) == ["0", "0", "1", "2"], out
        out, _ = run(vm, "calculator --eval '2+3' 'ans*2'")
        assert lines(out) == ["5", "10"], out

        # Sakuya's Pocket Watch: calendar arithmetic
        out, _ = run(vm, "pocketwatch --date 2026-10-23; pocketwatch --date 2024-02-29; pocketwatch --date 2021-01-03; pocketwatch --date 2025-12-29")
        assert lines(out) == ["Friday day 296 week 2026-W43", "Thursday day 60 week 2024-W09", "Sunday day 3 week 2020-W53", "Monday day 363 week 2026-W01"], out
        out, _ = run(vm, "pocketwatch --calendar 2026 10")
        assert "October 2026" in out and "Wk Mo Tu We Th Fr Sa Su" in out, out
        assert re.search(r"^43\s+19 20 21 22 23 24 25$", out, re.M), out
        out, rc = run(vm, "pocketwatch --date 2023-02-29")
        assert rc == 2, out

        # Spell Manager: /proc parsing
        out, _ = run(vm, "spellmgr --dump")
        assert re.search(r"^\s+1 init\s+root", out, re.M) and "spellmgr" in out and re.search(r"\d+ spells", out), out
        out, _ = run(vm, "spellmgr --hardware")
        for needle in ("== Processor ==", "== Memory ==", "== PCI devices ==", "VGA compatible controller", "== Mounted volumes ==", "/proc | procfs"):
            assert needle in out, f"missing {needle!r} in:\n{out}"
        out, _ = run(vm, "spellmgr --users")
        assert "reimu (you) | 1000" in out and "shrine | 100" in out, out
        out, _ = run(vm, "spellmgr --perf")
        assert re.search(r"cpu \d+% memory \d+% uptime \d+ ms", out), out

        # Kappa Control Panel: ~/.config/hakurei.conf
        out, rc = run(vm, "rm -f ~/.config/hakurei.conf; kappa-panel --conf get clock24")
        assert rc == 1, out
        run(vm, "kappa-panel --conf set clock24 0")
        out, _ = run(vm, "cat ~/.config/hakurei.conf")
        assert "clock24=0" in out, out
        run(vm, "kappa-panel --conf set theme shrine; kappa-panel --conf set clock24 1")
        out, _ = run(vm, "cat ~/.config/hakurei.conf; kappa-panel --conf get theme")
        assert lines(out)[-3:] == ["clock24=1", "theme=shrine", "shrine"], out
        out, _ = run(vm, "kappa-panel --devices")
        assert "== PCI devices ==" in out and "Host bridge" in out, out

        # Bunbunmaru News: the paper
        out, _ = run(vm, "bunbunmaru --pool")
        m = re.search(r"stories (\d+) classifieds (\d+) spellcards (\d+)", out)
        assert m and int(m.group(1)) >= 40 and int(m.group(2)) >= 20 and int(m.group(3)) >= 12, out
        system_kickers = ("FRONT PAGE", "THE LARDER", "SPELLS", "VOLUMES", "THE PCI QUARTER", "HARDWARE", "THE COURT", "THE KERNEL LOG")

        def stories(date):
            text, _ = run(vm, f"bunbunmaru --print --date {date} | grep '^\\['", timeout=90)
            heads = lines(text)
            return heads, [h for h in heads if not h.startswith(tuple("[" + k + "]" for k in system_kickers))]

        heads23, gossip23 = stories("2026-10-23")
        heads23b, gossip23b = stories("2026-10-23")
        heads24, gossip24 = stories("2026-10-24")
        assert len(gossip23) >= 20, heads23
        assert gossip23 == gossip23b, "the same date must give the same stories"
        assert gossip23 != gossip24, "different days must give different papers"
        assert any(h.startswith("[FRONT PAGE]") for h in heads23), heads23
        text, _ = run(vm, "bunbunmaru --print --date 2026-10-23 | head -3", timeout=60)
        assert "THE BUNBUNMARU NEWS" in text and "Friday, 23 October 2026" in text, text
        assert re.search(r"\[SPELLS\] \d+ Spells? In Flight", "\n".join(heads23)), heads23
        assert re.search(r"\[VOLUMES\] \d+ Volumes? Open For Business", "\n".join(heads23)), heads23
        assert re.search(r"\[THE LARDER\] .*(Yuyuko|Larder)", "\n".join(heads23)), heads23


# ---------------------------------------------------------------- windows

def wait_pixels(vm, predicate, timeout=60, step=1.5):
    end = time.time() + timeout
    shot = None
    while time.time() < end:
        shot = vm.screen_ppm()
        if predicate(shot):
            return shot
        time.sleep(step)
    raise AssertionError("the screen never looked as expected")


def count_near(shot, colors, tol=10, step=3):
    w, h, rgb = shot
    n = 0
    for y in range(0, h, step):
        row = y * w * 3
        for x in range(0, w, step):
            i = row + x * 3
            r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
            for cr, cg, cb in colors:
                if abs(r - cr) <= tol and abs(g - cg) <= tol and abs(b - cb) <= tol:
                    n += 1
                    break
    return n


PAPER = [(0xEC, 0xE6, 0xD6), (0xF3, 0xEE, 0xDC), (0xF7, 0xF3, 0xE6)]


def start_desktop(vm):
    vm.send("fairyd &\n")
    vm.expect(r"fairyd: \d+x\d+", timeout=120)
    vm.send("hakurei-shell &\n")
    time.sleep(4)


def launch(vm, cmd, ready):
    vm.send(cmd + " &\n")
    vm.expect(ready, timeout=180)
    time.sleep(2.5)                                       # let the first frame reach the screen


@test
def apps_windows():
    """Each application opens a window on the desktop, reacts to the keyboard (checked on the serial trace) and draws paper."""
    with Qemu(cmdline="text", name="t-apps-gui", mem="768M") as vm:
        vm.expect(PROMPT, timeout=90)
        start_desktop(vm)

        launch(vm, "calculator --trace", r"calc: ready")
        vm.type("2+3*4\n")
        vm.expect(r"calc: 2\+3\*4 = 14", timeout=120)
        vm.type("*2\n")
        vm.expect(r"calc: ans\*2 = 28", timeout=120)
        vm.type("1/0\n")
        vm.expect(r"calc: 1/0 -> error: Division by zero", timeout=120)
        vm.key("esc")
        vm.key("c", ctrl=True)                           # copies the last answer (28)
        vm.expect(r"calc: copied 28", timeout=120)
        vm.key("v", ctrl=True)                           # pastes it back as an expression
        vm.type("+1\n")
        vm.expect(r"calc: 28\+1 = 29", timeout=120)
        shot = wait_pixels(vm, lambda s: count_near(s, PAPER) > 4000)
        vm.screenshot(BUILD / "t-apps-calculator.png")

        launch(vm, "pocketwatch --trace --tab stopwatch", r"pocketwatch: ready")
        vm.key("spc")
        vm.expect(r"pocketwatch: stopwatch started", timeout=120)
        vm.key("l")
        vm.expect(r"pocketwatch: lap 1 at", timeout=120)
        vm.key("spc")
        vm.expect(r"pocketwatch: stopwatch stopped at", timeout=120)
        vm.key("1", ctrl=True)
        vm.expect(r"pocketwatch: tab clock", timeout=120)
        vm.key("t")
        vm.expect(r"Sakuya has stopped time", timeout=120)
        vm.key("t")
        vm.expect(r"time flows again", timeout=120)
        vm.key("4", ctrl=True)
        vm.expect(r"pocketwatch: tab calendar", timeout=120)
        vm.screenshot(BUILD / "t-apps-pocketwatch.png")

        launch(vm, "spellmgr --trace", r"spellmgr: ready with \d+ spells")
        vm.key("down")
        vm.expect(r"spellmgr: selected init", timeout=120)
        vm.key("2", ctrl=True)
        vm.expect(r"spellmgr: tab performance", timeout=120)
        vm.key("3", ctrl=True)
        vm.expect(r"spellmgr: hardware page has \d+ lines", timeout=120)
        vm.key("4", ctrl=True)
        vm.expect(r"spellmgr: tab users", timeout=120)
        vm.screenshot(BUILD / "t-apps-spellmgr.png")

        launch(vm, "kappa-panel --trace --pane input", r"kappa-panel: ready")
        vm.key("a")
        vm.expect(r"kappa-panel: key 1e", timeout=120)
        vm.key("2", alt=True)
        vm.expect(r"kappa-panel: pane display", timeout=120)
        vm.key("7", alt=True)
        vm.expect(r"kappa-panel: pane appearance", timeout=120)
        vm.screenshot(BUILD / "t-apps-kappa-panel.png")

        launch(vm, "bunbunmaru --trace", r"bunbunmaru: ready")
        vm.key("3")
        vm.expect(r"bunbunmaru: page 3 Around Gensokyo", timeout=120)
        vm.key("right")
        vm.expect(r"bunbunmaru: page 4 Features", timeout=120)
        vm.key("end")
        vm.expect(r"bunbunmaru: page 5 Classifieds", timeout=120)
        vm.key("home")
        vm.expect(r"bunbunmaru: page 1 Front page", timeout=120)
        wait_pixels(vm, lambda s: count_near(s, [(0xF3, 0xEE, 0xDC)], tol=4) > 8000)
        vm.screenshot(BUILD / "t-apps-bunbunmaru.png")
