"""USB host stack: xHCI + hubs + HID keyboards and mice, hot-plug, and the "nothing there" cases (QEMU's qemu-xhci).

QEMU routes injected keyboard events to the most recently plugged USB keyboard and mouse events to the mouse the guest is polling,
so no explicit device selection is needed (input-send-event's `device` argument names a display, not an input device).
The kernel exposes two text files for these tests: /proc/usb (the device tree, what `lsusb` prints) and /proc/usbstat
(counters of what the HID drivers fed into the input layer, plus the input layer's own mouse position and buttons).
"""
import re
import time

from harness import test, Qemu, BUILD
from test_shell import run, PROMPT

XHCI = ["-device", "qemu-xhci,id=xhci"]
KBD = ["-device", "usb-kbd,id=ukbd"]
MOUSE = ["-device", "usb-mouse,id=umouse"]
KBD_UP = r"\[Satori\] usb \S+: keyboard online \(interface"
MOUSE_UP = r"\[Koishi\] usb \S+: mouse online \(interface"
GONE = r"\[Kappa\] usb \S+: device disconnected"


# ---------------------------------------------------------------- helpers

def usb_vm(devices, cmdline="", name="t-usb", xhci=True):
    return Qemu(cmdline=cmdline, extra=(XHCI if xhci else []) + list(devices), name=name)


def wait_count(vm, pattern, n=1, timeout=60):
    """Waits until the serial log holds at least n matches of pattern (searches the whole log, not just new output)."""
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        if len(rx.findall(vm.text())) >= n:
            return
        if not vm.alive():
            break
        time.sleep(0.1)
    raise TimeoutError(f"expected {n} x /{pattern}/ in the log, got {len(rx.findall(vm.text()))}:\n" + vm.text()[-2500:])


def boot(vm, keyboards=1, mice=1):
    """Waits for the shell and for the USB drivers to bind the expected number of keyboards and mice."""
    vm.expect(PROMPT, timeout=90)
    wait_count(vm, KBD_UP, keyboards)
    wait_count(vm, MOUSE_UP, mice)
    wait_count(vm, r"\[Kappa\] usb \S+: new \S+ device", keyboards + mice)
    time.sleep(0.5)


def stat(vm):
    out, rc = run(vm, "cat /proc/usbstat")
    assert rc == 0, out
    return {k: int(v) for k, v in re.findall(r"^(\w+): (-?\d+)\s*$", out, re.M)}


def devices(vm):
    """The device lines of /proc/usb."""
    out, rc = run(vm, "cat /proc/usb")
    assert rc == 0, out
    return [ln for ln in out.splitlines() if ln.startswith("Bus ") and " Port " in ln]


def type_run(vm, text, expect, timeout=30):
    """Types `text` (ending in a newline) through the USB keyboard and waits for the shell's answer."""
    vm.type(text)
    m = vm.expect(expect, timeout=timeout)
    vm.expect(PROMPT, timeout=20)
    return m


def clear_line(vm):
    """Throws away whatever is on the command line (Ctrl+C through the serial console) and waits for a fresh prompt."""
    vm.send("\x03")
    vm.expect(PROMPT, timeout=20)


def key_event(vm, qcode, down):
    vm._input([vm._key_event(qcode, down)])


def no_errors(vm):
    text = vm.text()
    for bad in ("setup failed", "giving up", "start-up failed", "timed out", "keeps failing", "INCIDENT"):
        assert bad not in text, f"unexpected {bad!r} in the log:\n" + text[-2500:]


# ---------------------------------------------------------------- enumeration

@test
def usb_enumerate():
    """xHCI + keyboard + mouse: /proc/usb, lsusb, lspci and the log describe them; nothing went wrong."""
    with usb_vm(KBD + MOUSE, name="t-usb-enum") as vm:
        boot(vm)
        out, rc = run(vm, "cat /proc/usb")
        assert re.search(r"^Bus 1: xHCI 1b36:000d at \S+, v1\.\d+, 8 ports \(4 usb2, 4 usb3\), \d+ slots", out, re.M), out
        assert re.search(r"^Bus 1 Port \d+: ID 0627:0001 QEMU QEMU USB Keyboard, high-speed, class 00, driver hid-keyboard$", out, re.M), out
        assert re.search(r"^Bus 1 Port \d+: ID 0627:0001 QEMU QEMU USB Mouse, high-speed, class 00, driver hid-mouse$", out, re.M), out
        out, rc = run(vm, "lsusb")
        assert rc == 0 and "USB Keyboard" in out and "USB Mouse" in out, out
        out, rc = run(vm, "lspci | grep xHCI")
        assert "[xhci]" in out, out
        out, rc = run(vm, "lsusb | grep -c '^Bus.*driver hid'")
        assert out.strip() == "2", out
        no_errors(vm)


@test
def usb_superspeed():
    """A SuperSpeed device on a USB 3 root port enumerates (no driver binds); the keyboard on a USB 2 port still works."""
    img = BUILD / "usb-storage-test.img"
    img.write_bytes(b"\0" * (1 << 20))
    extra = ["-drive", f"if=none,id=usbd,format=raw,file={img}", "-device", "usb-storage,drive=usbd,id=ustor"] + KBD
    with usb_vm(extra, name="t-usb-ss") as vm:
        vm.expect(PROMPT, timeout=90)
        wait_count(vm, KBD_UP, 1)
        wait_count(vm, r"\[Kappa\] usb \S+: new super-speed device 46f4:0001", 1)
        lines = devices(vm)
        assert any("super-speed" in ln and "46f4:0001" in ln and "driver none" in ln for ln in lines), lines
        assert any("high-speed" in ln and "hid-keyboard" in ln for ln in lines), lines
        no_errors(vm)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")


@test
def usb_tablet_declined():
    """An absolute pointing device (usb-tablet) is listed but not bound; the keyboard next to it works."""
    with usb_vm(["-device", "usb-tablet,id=utab"] + KBD, name="t-usb-tablet") as vm:
        vm.expect(PROMPT, timeout=90)
        wait_count(vm, KBD_UP, 1)
        wait_count(vm, r"\[Kappa\] usb \S+: new \S+ device 0627:0001 \"QEMU QEMU USB Tablet\"", 1)
        lines = devices(vm)
        assert any("Tablet" in ln and "driver none" in ln for ln in lines), lines
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        no_errors(vm)


# ---------------------------------------------------------------- keyboard

@test
def usb_keyboard_text():
    """Typing through the USB keyboard reaches the gapsh prompt (letters, digits, punctuation, shifted symbols)."""
    msg = "Hello, Gensokyo: [a-z] {0-9} ~ ^ & ; # @ % ! | < > ? _ + = - / \" $ ( ) * ."
    with usb_vm(KBD + MOUSE, name="t-usb-kbd") as vm:
        boot(vm)
        s0 = stat(vm)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        type_run(vm, f"echo '{msg}'\n", re.escape(msg) + r"\r?\n")
        s1 = stat(vm)
        assert s1["key_down"] > s0["key_down"] + 60, (s0, s1)         # the characters really came through the USB path
        assert s1["keys_held"] == 0, s1
        assert s1["kbd_reports"] >= (s1["key_down"] - s0["key_down"]), s1
        no_errors(vm)


@test
def usb_keyboard_modifiers():
    """Shift (letters and symbols), Ctrl chords (Ctrl+W, Ctrl+C), arrows, Delete, Home/End, keypad and history."""
    with usb_vm(KBD + MOUSE, name="t-usb-mods") as vm:
        boot(vm)
        type_run(vm, "echo ABC Def GHI $((2*3))\n", r"\nABC Def GHI 6\r?\n")
        # Ctrl+W deletes the last word
        vm.type("echo one two")
        vm.key("w", ctrl=True)
        type_run(vm, "three\n", r"\none three\r?\n")
        # Ctrl+C abandons the line
        vm.type("echo never")
        vm.key("c", ctrl=True)
        vm.expect(r"\^C", timeout=15)
        vm.expect(PROMPT, timeout=20)
        type_run(vm, "echo $((1+1))\n", r"\n2\r?\n")
        # Left arrow and Delete edit inside the line
        vm.type("echo ac")
        vm.key("left")
        vm.type("b")
        type_run(vm, "\n", r"\nabc\r?\n")
        vm.type("echo abXc")
        vm.key("left")
        vm.key("left")
        vm.key("delete")
        type_run(vm, "\n", r"\nabc\r?\n")
        # Home / End
        vm.type("echo bc")
        vm.key("home")
        vm.type("echo ")                                                # becomes "echo echo bc"
        vm.key("end")
        type_run(vm, "\n", r"\necho bc\r?\n")
        # keypad with NumLock on: digits and Enter
        vm.type("echo ")
        for k in ("kp_1", "kp_2", "kp_3"):
            vm.key(k)
        vm.key("kp_enter")
        vm.expect(r"\n123\r?\n", timeout=20)
        vm.expect(PROMPT, timeout=20)
        # Up arrow recalls the previous command
        vm.key("up")
        vm.key("ret")
        vm.expect(r"\n123\r?\n", timeout=20)
        vm.expect(PROMPT, timeout=20)
        no_errors(vm)


@test
def usb_keyboard_keymap():
    """Every key class maps to something: F1-F12, navigation, GUI, menu, modifiers and keypad each produce a key event."""
    keys = (["f%d" % i for i in range(1, 13)] + ["pgup", "pgdn", "insert", "home", "end", "delete", "up", "down", "left", "right",
            "meta_l", "meta_r", "caps_lock", "caps_lock", "num_lock", "num_lock", "scroll_lock", "scroll_lock",
            "kp_0", "kp_5", "kp_9", "kp_divide", "kp_multiply", "kp_subtract", "kp_add", "kp_decimal",
            "ctrl_r", "alt_r", "shift_r", "esc", "tab", "backspace", "spc", "minus", "equal", "bracket_left", "bracket_right",
            "backslash", "semicolon", "apostrophe", "grave_accent", "comma", "dot", "slash"])
    with usb_vm(KBD + MOUSE, name="t-usb-keymap") as vm:
        boot(vm)
        s0 = stat(vm)
        for k in keys:                                                  # no Enter among them: the line editor only edits
            vm.key(k, hold=0.06)
        time.sleep(1.0)
        clear_line(vm)
        s1 = stat(vm)
        n = s1["key_down"] - s0["key_down"]
        assert n == len(keys), f"{n} key presses seen for {len(keys)} keys sent: {s0} {s1}"
        assert s1["key_up"] - s0["key_up"] == len(keys), (s0, s1)
        assert s1["keys_held"] == 0, s1
        assert s1["leds"] == s0["leds"] == 1, (s0, s1)                  # lock keys were pressed an even number of times
        no_errors(vm)


@test
def usb_key_repeat():
    """Holding a key on the USB keyboard repeats it (the input layer's typematic generator)."""
    with usb_vm(KBD + MOUSE, name="t-usb-repeat") as vm:
        boot(vm)
        s0 = stat(vm)
        key_event(vm, "x", True)
        time.sleep(4.0)
        key_event(vm, "x", False)
        time.sleep(0.5)
        text = vm.text()
        runs = [len(r) for r in re.findall(r"\$ (x{1,300})", text)]
        longest = max(runs) if runs else 0
        assert longest >= 4, f"expected a repeated run of x on the command line, longest run {longest}:\n" + text[-400:]
        clear_line(vm)
        s1 = stat(vm)
        assert s1["key_down"] == s0["key_down"] + 1 and s1["keys_held"] == 0, (s0, s1)


# ---------------------------------------------------------------- mouse

@test
def usb_mouse():
    """USB mouse: motion, buttons and wheel reach the input layer."""
    with usb_vm(KBD + MOUSE, name="t-usb-mouse") as vm:
        boot(vm)
        assert vm.select_mouse("HID") is not None
        s0 = stat(vm)
        vm.mouse_move(40, 25)
        time.sleep(0.3)
        vm.mouse_move(-10, 0)
        time.sleep(0.3)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 30 and s1["mouse_dy"] - s0["mouse_dy"] == 25, (s0, s1)
        assert s1["mouse_x"] - s0["mouse_x"] == 30 and s1["mouse_y"] - s0["mouse_y"] == 25, (s0, s1)
        vm.mouse_button("left", True)
        time.sleep(0.3)
        assert stat(vm)["mouse_buttons"] == 1
        vm.mouse_button("right", True)
        time.sleep(0.3)
        assert stat(vm)["mouse_buttons"] == 3
        vm.mouse_button("left", False)
        vm.mouse_button("right", False)
        time.sleep(0.3)
        assert stat(vm)["mouse_buttons"] == 0
        vm.mouse_button("middle", True)
        time.sleep(0.3)
        assert stat(vm)["mouse_buttons"] == 4
        vm.mouse_button("middle", False)
        time.sleep(0.3)
        s2 = stat(vm)
        vm.mouse_button("wheel-up", True)
        vm.mouse_button("wheel-up", False)
        time.sleep(0.4)
        s3 = stat(vm)
        vm.mouse_button("wheel-down", True)
        vm.mouse_button("wheel-down", False)
        time.sleep(0.4)
        s4 = stat(vm)
        up, down = s3["mouse_wheel"] - s2["mouse_wheel"], s4["mouse_wheel"] - s3["mouse_wheel"]
        assert abs(up) == 1 and down == -up, (up, down)
        assert s4["mouse_buttons"] == 0, s4
        no_errors(vm)


@test
def usb_report_protocol():
    """usb=hidreport parses the report descriptors even for boot interfaces: keyboard and mouse still work."""
    with usb_vm(KBD + MOUSE, cmdline="usb=hidreport", name="t-usb-report") as vm:
        boot(vm)
        text = vm.text()
        assert re.search(r"keyboard online \(interface 0, report protocol\)", text), text[-2000:]
        assert re.search(r"mouse online \(interface 0, report protocol, wheel\)", text), text[-2000:]
        type_run(vm, "echo ABC $((6*7))\n", r"\nABC 42\r?\n")
        vm.select_mouse("HID")
        s0 = stat(vm)
        vm.mouse_move(12, -7)
        vm.mouse_button("left", True)
        time.sleep(0.5)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 12 and s1["mouse_dy"] - s0["mouse_dy"] == -7 and s1["mouse_buttons"] == 1, (s0, s1)
        vm.mouse_button("left", False)


# ---------------------------------------------------------------- hubs

@test
def usb_hub():
    """Keyboard and mouse behind a hub: tree, speeds, typing and mouse motion; unplugging the hub takes both away."""
    extra = ["-device", "usb-hub,id=hub1,bus=xhci.0,port=1", "-device", "usb-kbd,id=ukbd,bus=xhci.0,port=1.1",
             "-device", "usb-mouse,id=umouse,bus=xhci.0,port=1.2"]
    with usb_vm(extra, name="t-usb-hub") as vm:
        boot(vm)
        out, rc = run(vm, "lsusb")
        assert re.search(r"^Bus 1 Port (\d+): ID 0409:55aa QEMU QEMU USB Hub, full-speed, class 09, driver hub$", out, re.M), out
        hub_port = re.search(r"^Bus 1 Port (\d+): ID 0409:55aa", out, re.M).group(1)
        assert re.search(rf"^Bus 1 Port {hub_port}\.1: ID 0627:0001 QEMU QEMU USB Keyboard, full-speed, .*driver hid-keyboard$", out, re.M), out
        assert re.search(rf"^Bus 1 Port {hub_port}\.2: ID 0627:0001 QEMU QEMU USB Mouse, full-speed, .*driver hid-mouse$", out, re.M), out
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        vm.select_mouse("HID")
        s0 = stat(vm)
        vm.mouse_move(15, 9)
        time.sleep(0.5)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 15 and s1["mouse_dy"] - s0["mouse_dy"] == 9, (s0, s1)
        no_errors(vm)
        # pull the hub out: the children go with it
        vm.qmp("device_del", id="hub1")
        wait_count(vm, GONE, 3, timeout=30)
        time.sleep(0.5)
        assert devices(vm) == [], devices(vm)
        s = stat(vm)
        assert s["keyboards"] == 0 and s["mice"] == 0, s
        # put a hub back with a keyboard behind it
        vm.qmp("device_add", driver="usb-hub", id="hub2", bus="xhci.0", port="2")
        vm.qmp("device_add", driver="usb-kbd", id="ukbd2", bus="xhci.0", port="2.3")
        wait_count(vm, KBD_UP, 2, timeout=40)
        lines = devices(vm)
        assert len(lines) == 2 and any("hid-keyboard" in ln and "." in ln.split(":")[0] for ln in lines), lines
        type_run(vm, "echo $((7*7))\n", r"\n49\r?\n")
        no_errors(vm)


# ---------------------------------------------------------------- hot plug

@test
def usb_hotplug():
    """Unplug and replug a keyboard and a mouse; keys held at unplug time are released, the new devices work."""
    with usb_vm(KBD + MOUSE, name="t-usb-hotplug") as vm:
        boot(vm)
        # hold a key, pull the keyboard: the key must be released and the repeat must stop
        s0 = stat(vm)
        key_event(vm, "z", True)
        time.sleep(1.0)                                                 # (the repeating z's fill the command line: no shell commands now)
        vm.qmp("device_del", id="ukbd")
        wait_count(vm, GONE, 1, timeout=30)
        time.sleep(0.5)
        t1 = vm.text().count("z")
        time.sleep(1.5)
        assert vm.text().count("z") == t1, "key repeat went on after the keyboard was unplugged"
        clear_line(vm)
        s = stat(vm)
        assert s["keys_held"] == 0 and s["keyboards"] == 0, s
        assert s["key_down"] == s0["key_down"] + 1 and s["key_up"] == s0["key_up"] + 1, (s0, s)       # the release was synthesised
        lines = devices(vm)
        assert len(lines) == 1 and "USB Mouse" in lines[0], lines
        # plug a keyboard again (any port will do)
        vm.qmp("device_add", driver="usb-kbd", id="ukbd2", bus="xhci.0")
        wait_count(vm, KBD_UP, 2, timeout=40)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        # mouse: hold a button, unplug, the button must be released
        vm.select_mouse("HID")
        vm.mouse_button("left", True)
        time.sleep(0.5)
        assert stat(vm)["mouse_buttons"] == 1
        vm.qmp("device_del", id="umouse")
        wait_count(vm, GONE, 2, timeout=30)
        time.sleep(0.5)
        s = stat(vm)
        assert s["mouse_buttons"] == 0 and s["mice"] == 0, s
        vm.qmp("device_add", driver="usb-mouse", id="umouse2", bus="xhci.0")
        wait_count(vm, MOUSE_UP, 2, timeout=40)
        time.sleep(0.5)
        vm.select_mouse("HID")
        s0 = stat(vm)
        vm.mouse_move(20, 20)
        time.sleep(0.5)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 20, (s0, s1)
        assert len(devices(vm)) == 2
        no_errors(vm)


@test
def usb_two_keyboards():
    """Two keyboards at once: both enumerate, typing works, and after the active one is unplugged the other takes over."""
    kbds = ["-device", "usb-kbd,id=k1", "-device", "usb-kbd,id=k2"]
    with usb_vm(kbds + MOUSE, name="t-usb-2kbd") as vm:
        boot(vm, keyboards=2)
        assert stat(vm)["keyboards"] == 2
        assert len([ln for ln in devices(vm) if "hid-keyboard" in ln]) == 2
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")           # goes to the most recently plugged keyboard (k2)
        vm.qmp("device_del", id="k2")
        wait_count(vm, GONE, 1, timeout=30)
        time.sleep(0.5)
        assert stat(vm)["keyboards"] == 1
        type_run(vm, "echo $((7*8))\n", r"\n56\r?\n")           # now k1 gets the events
        no_errors(vm)


# ---------------------------------------------------------------- control pipe recovery

@test
def usb_stall_recovery():
    """A STALLed control request halts the pipe; the driver clears it (Reset Endpoint + Set TR Dequeue Pointer) and carries on."""
    with usb_vm(KBD + MOUSE, cmdline="usb=stalltest", name="t-usb-stall") as vm:
        boot(vm)
        wait_count(vm, r"stall test: bogus request returned -32, device descriptor afterwards returned 18", 2, timeout=30)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        assert "INCIDENT" not in vm.text()


# ---------------------------------------------------------------- nothing to drive

@test
def usb_xhci_empty():
    """An xHCI controller with nothing plugged in: boots normally, lists no devices, PS/2 keyboard unaffected."""
    with usb_vm([], name="t-usb-empty") as vm:
        vm.expect(PROMPT, timeout=90)
        wait_count(vm, r"\[Kappa\] xHCI 1b36:000d at ", 1)
        time.sleep(1.0)
        out, rc = run(vm, "lsusb")
        assert rc == 0 and out.startswith("Bus 1: xHCI") and "Port" not in out, out
        assert stat(vm)["keyboards"] == 0
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")           # reaches the shell through the PS/2 keyboard
        no_errors(vm)


@test
def usb_absent():
    """No USB controller at all (PS/2 only): boots, lsusb says so, the PS/2 keyboard works."""
    with usb_vm([], name="t-usb-none", xhci=False) as vm:
        vm.expect(PROMPT, timeout=90)
        out, rc = run(vm, "lsusb")
        assert rc == 1 and "no USB subsystem" in out, (rc, out)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")


@test
def usb_off():
    """usb=off: the controller is left alone, there is no /proc/usb, and the PS/2 keyboard still works."""
    # (a USB keyboard would become QEMU's default key target and swallow the keystrokes, so only a mouse is attached)
    with usb_vm(MOUSE, cmdline="usb=off", name="t-usb-off") as vm:
        vm.expect(PROMPT, timeout=90)
        out, rc = run(vm, "lsusb")
        assert rc == 1 and "no USB subsystem" in out, (rc, out)
        assert "disabled on the kernel command line" in vm.text()
        assert "mouse online (interface" not in vm.text()
        out, rc = run(vm, "lspci | grep xHCI")
        assert "[xhci]" not in out, out
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
