"""USB host stack: xHCI + hubs + HID keyboards and mice, hot-plug, error recovery, and the "nothing there" cases (QEMU's qemu-xhci).

QEMU routes injected keyboard events to the most recently plugged USB keyboard and mouse events to the mouse the guest is polling,
so no explicit device selection is needed (input-send-event's `device` argument names a display, not an input device).
The kernel exposes two text files for these tests: /proc/usb (the device tree, what `lsusb` prints) and /proc/usbstat
(counters of what the HID drivers fed into the input layer, plus the input layer's own mouse position and buttons).

The keyboard and mouse scenarios run twice: with the devices on root ports and behind a (full-speed) usb-hub.
"""
import os
import re
import time

from harness import test, TESTS, Qemu, BUILD, scaled
from test_shell import run, PROMPT

XHCI = ["-device", "qemu-xhci,id=xhci"]
KBD = ["-device", "usb-kbd,id=ukbd"]
MOUSE = ["-device", "usb-mouse,id=umouse"]
BEHIND_HUB = ["-device", "usb-hub,id=hub1,bus=xhci.0,port=1", "-device", "usb-kbd,id=ukbd,bus=xhci.0,port=1.1",
              "-device", "usb-mouse,id=umouse,bus=xhci.0,port=1.2"]
KBD_UP = r"\[Satori\] usb \S+: keyboard online \(interface"
MOUSE_UP = r"\[Koishi\] usb \S+: mouse online \(interface"
GONE = r"\[Kappa\] usb \S+: device disconnected"
NEW_DEVICE = r"\[Kappa\] usb \S+: new \S+ device"


# ---------------------------------------------------------------- helpers

def usb_vm(devices, cmdline="", name="t-usb", xhci=True):
    ring = os.environ.get("USB_RING")                    # USB_RING=16 python tests/run_all.py usb_  runs everything on 15 entry rings
    if ring and "usbring" not in cmdline:
        cmdline = (cmdline + f" usbring={ring}").strip()
    return Qemu(cmdline=("text " + cmdline).strip(), extra=(XHCI if xhci else []) + list(devices), name=name)


def wait_count(vm, pattern, n=1, timeout=60):
    """Waits until the serial log holds at least n matches of pattern (searches the whole log, not just new output)."""
    rx = re.compile(pattern)
    end = time.time() + scaled(timeout)
    while time.time() < end:
        if len(rx.findall(vm.text())) >= n:
            return
        if not vm.alive():
            break
        time.sleep(0.1)
    raise TimeoutError(f"expected {n} x /{pattern}/ in the log, got {len(rx.findall(vm.text()))}:\n" + vm.text()[-2500:])


def boot(vm, keyboards=1, mice=1, hubs=0, timeout=60):
    """Waits for the shell and for the USB drivers to bind the expected number of keyboards, mice and hubs."""
    vm.expect(PROMPT, timeout=90)
    wait_count(vm, KBD_UP, keyboards, timeout)
    wait_count(vm, MOUSE_UP, mice, timeout)
    wait_count(vm, NEW_DEVICE, keyboards + mice + hubs, timeout)
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


def both_topologies(doc):
    """Registers the scenario twice: devices on root ports, and devices behind a hub. The scenario gets (devices, hubs, vm name)."""
    def deco(fn):
        for suffix, devs, hubs in (("", KBD + MOUSE, 0), ("_hub", BEHIND_HUB, 1)):
            def case(devs=devs, hubs=hubs, suffix=suffix):
                fn(devs, hubs, f"t-{fn.__name__}{suffix}".replace("_", "-"))
            case.__name__ = fn.__name__ + suffix
            case.__doc__ = doc + (" (behind a hub)" if suffix else "")
            TESTS.append(case)
        return fn
    return deco


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
def usb_nec_controller():
    """The same devices on QEMU's other xHCI model (NEC uPD720200 personality) enumerate and work."""
    with Qemu(cmdline="text", extra=["-device", "nec-usb-xhci,id=xhci"] + KBD + MOUSE, name="t-usb-nec") as vm:
        boot(vm)
        assert "xHCI 1033:0194" in vm.text()
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        no_errors(vm)


@test
def usb_fullspeed_devices():
    """Full-speed devices (8 byte control packets, so the default 64 must be corrected with Evaluate Context) on root ports and a hub."""
    extra = ["-device", "usb-kbd,id=ukbd,usb_version=1", "-device", "usb-mouse,id=umouse,usb_version=1"]
    with usb_vm(extra, cmdline="usb=debug", name="t-usb-fs") as vm:
        boot(vm)
        text = vm.text()
        assert len(re.findall(r"new full-speed device 0627:0001", text)) == 2, text[-2000:]
        assert len(re.findall(r"control endpoint packet size is 8, not the 64 we assumed", text)) == 2, text[-2000:]
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        vm.select_mouse("HID")
        s0 = stat(vm)
        vm.mouse_move(3, 4)
        time.sleep(0.5)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 3 and s1["mouse_dy"] - s0["mouse_dy"] == 4, (s0, s1)


@test
def usb_hub_port_power():
    """A hub with per-port power switching: ports stay dark until the driver powers them, then the devices behind appear."""
    extra = ["-device", "usb-hub,id=hub1,bus=xhci.0,port=1,port-power=on", "-device", "usb-kbd,id=ukbd,bus=xhci.0,port=1.1",
             "-device", "usb-mouse,id=umouse,bus=xhci.0,port=1.2"]
    with usb_vm(extra, name="t-usb-hubpower") as vm:
        boot(vm, hubs=1)
        assert len(devices(vm)) == 3
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        no_errors(vm)


@test
def usb_superspeed():
    """A SuperSpeed device (a mass storage stick) on a USB 3 root port enumerates and is bound by the usb-storage driver; the keyboard on a USB 2 port still works."""
    img = BUILD / "usb-storage-test.img"
    img.write_bytes(b"\0" * (1 << 20))
    extra = ["-drive", f"if=none,id=usbd,format=raw,file={img}", "-device", "usb-storage,drive=usbd,id=ustor"] + KBD
    with usb_vm(extra, name="t-usb-ss") as vm:
        vm.expect(PROMPT, timeout=90)
        wait_count(vm, KBD_UP, 1)
        wait_count(vm, r"\[Kappa\] usb \S+: new super-speed device 46f4:0001", 1)
        lines = devices(vm)
        assert any("super-speed" in ln and "46f4:0001" in ln and "driver usb-storage" in ln for ln in lines), lines
        assert any("high-speed" in ln and "hid-keyboard" in ln for ln in lines), lines
        no_errors(vm)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")


@test
def usb_bulk_transfers():
    """Bulk endpoints (SuperSpeed direct, full-speed behind a hub): INQUIRY, READ CAPACITY and a 64 KiB READ(10) over bulk-only transport."""
    img = BUILD / "usb-bulk-test.img"
    data = bytes(((i * 31) + (i >> 9)) & 0xFF for i in range(1 << 20))
    img.write_bytes(data)
    want = sum(data[512:512 + 65536])
    drive = ["-drive", f"if=none,id=usbd,format=raw,file={img}"]
    cases = (
        ("super-speed", drive + ["-device", "usb-storage,drive=usbd,id=ustor"]),
        ("full-speed", drive + ["-device", "usb-hub,id=hub1,bus=xhci.0,port=1",
                                "-device", "usb-storage,drive=usbd,id=ustor,bus=xhci.0,port=1.1"]),
    )
    for speed, extra in cases:
        with usb_vm(extra, cmdline="usb=bulktest", name="t-usb-bulk") as vm:
            vm.expect(PROMPT, timeout=90)
            wait_count(vm, r"bulk test: READ\(10\) of 64 KiB ok", 1, timeout=60)
            text = vm.text()
            assert re.search(r"bulk test: INQUIRY -> 'QEMU    ' 'QEMU HARDDISK   '", text), text[-1500:]
            assert re.search(r"bulk test: last block 2047, block size 512", text), text[-1500:]
            m = re.search(r"READ\(10\) of 64 KiB ok, byte sum (\d+)", text)
            assert int(m.group(1)) == want, (speed, m.group(1), want)
            assert f"new {speed} device 46f4:0001" in text, (speed, text[-1500:])
            no_errors(vm)


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

@both_topologies("Typing through the USB keyboard reaches the gapsh prompt (letters, digits, punctuation, shifted symbols).")
def usb_keyboard_text(devs, hubs, name):
    msg = "Hello, Gensokyo: [a-z] {0-9} ~ ^ & ; # @ % ! | < > ? _ + = - / \" $ ( ) * ."
    with usb_vm(devs, name=name) as vm:
        boot(vm, hubs=hubs)
        s0 = stat(vm)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        type_run(vm, f"echo '{msg}'\n", re.escape(msg) + r"\r?\n")
        s1 = stat(vm)
        assert s1["key_down"] > s0["key_down"] + 60, (s0, s1)         # the characters really came through the USB path
        assert s1["keys_held"] == 0, s1
        assert s1["kbd_reports"] >= (s1["key_down"] - s0["key_down"]), s1
        no_errors(vm)


@both_topologies("Shift (letters and symbols), Ctrl chords (Ctrl+W, Ctrl+C), arrows, Delete, Home/End, keypad and history.")
def usb_keyboard_modifiers(devs, hubs, name):
    with usb_vm(devs, name=name) as vm:
        boot(vm, hubs=hubs)
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
    """Every key class maps to something: F1-F12, navigation, GUI, modifiers, locks and keypad each produce a key event."""
    keys = (["f%d" % i for i in range(1, 13)] + ["pgup", "pgdn", "insert", "home", "end", "delete", "up", "down", "left", "right",
            "meta_l", "meta_r", "caps_lock", "caps_lock", "num_lock", "num_lock", "scroll_lock", "scroll_lock",
            "kp_0", "kp_5", "kp_9", "kp_divide", "kp_multiply", "kp_subtract", "kp_add", "kp_decimal",
            "ctrl_r", "alt_r", "shift_r", "esc", "tab", "backspace", "spc", "minus", "equal", "bracket_left", "bracket_right",
            "backslash", "semicolon", "apostrophe", "grave_accent", "comma", "dot", "slash"])
    # (QEMU's usb-kbd has no usage for the "menu" key, so the Application key cannot be exercised here)
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


@both_topologies("Holding a key on the USB keyboard repeats it (the input layer's typematic generator).")
def usb_key_repeat(devs, hubs, name):
    with usb_vm(devs, name=name) as vm:
        boot(vm, hubs=hubs)
        s0 = stat(vm)
        key_event(vm, "x", True)
        # The repeat starts after 500 ms of *guest* time, which runs slower than the wall clock when the host is busy: wait for it.
        end = time.time() + scaled(120)
        while time.time() < end and not re.search(r"\$ x{6,}", vm.text()):
            time.sleep(0.25)
        key_event(vm, "x", False)
        time.sleep(0.5)
        text = vm.text()
        runs = [len(r) for r in re.findall(r"\$ (x{1,300})", text)]
        longest = max(runs) if runs else 0
        assert longest >= 6, f"expected a repeated run of x on the command line, longest run {longest}:\n" + text[-400:]
        clear_line(vm)
        s1 = stat(vm)
        assert s1["key_down"] == s0["key_down"] + 1 and s1["keys_held"] == 0, (s0, s1)


# ---------------------------------------------------------------- mouse

@both_topologies("USB mouse: motion, buttons and wheel reach the input layer.")
def usb_mouse(devs, hubs, name):
    with usb_vm(devs, name=name) as vm:
        boot(vm, hubs=hubs)
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
def usb_ring_wrap():
    """usbring=16 leaves 15 usable TRBs per ring: command, control, interrupt and event rings wrap over and over, nothing is lost."""
    with usb_vm(KBD + MOUSE, cmdline="usbring=16", name="t-usb-wrap") as vm:
        boot(vm)
        vm.select_mouse("HID")
        s0 = stat(vm)
        sent = 0
        s1 = s0
        # keep moving until the guest has seen several laps of the 15 entry rings (QEMU merges moves that arrive between two polls)
        while s1["mouse_reports"] - s0["mouse_reports"] < 80 and sent < 6000:
            for _ in range(40):
                vm.mouse_move(1, 2)
                time.sleep(0.02)
            sent += 40
            s1 = stat(vm)
        end = time.time() + scaled(60)
        while time.time() < end and s1["mouse_dx"] - s0["mouse_dx"] != sent:           # let the last reports drain
            time.sleep(0.5)
            s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == sent and s1["mouse_dy"] - s0["mouse_dy"] == 2 * sent, (sent, s0, s1)
        assert s1["mouse_reports"] - s0["mouse_reports"] >= 80, (s0, s1)
        keys = "abcdefghij" * 5
        for k in keys:
            vm.key(k, hold=0.02)
        end = time.time() + scaled(120)
        while time.time() < end and not re.search(r"\$ (?:abcdefghij){5}", vm.text()):       # all of it has reached the command line
            time.sleep(0.5)
        clear_line(vm)
        s2 = stat(vm)
        assert s2["key_down"] - s1["key_down"] == len(keys) and s2["keys_held"] == 0, (s1, s2)
        # unplug and plug again: more commands and control transfers through the tiny command and control rings
        for i in range(3):
            vm.qmp("device_del", id="umouse" if i == 0 else f"m{i}")
            wait_count(vm, GONE, i + 1, timeout=60)
            vm.qmp("device_add", driver="usb-mouse", id=f"m{i + 1}", bus="xhci.0")
            wait_count(vm, MOUSE_UP, i + 2, timeout=60)
        time.sleep(0.5)
        assert len(devices(vm)) == 2
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        no_errors(vm)


@test
def usb_hid_parser():
    """HID report descriptor parser and report decoders against descriptors of common mice and keyboards (kernel self test)."""
    with Qemu(cmdline="text usb=parsertest", name="t-usb-parser") as vm:
        m = vm.expect(r"hid parser self test: (\d+) checks, (\d+) failed", timeout=60)
        assert int(m.group(1)) >= 20 and m.group(2) == "0", vm.text()[-1500:]
        assert "FAILED" not in vm.text(), vm.text()[-1500:]


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
    """Devices behind a hub: tree, speeds; unplug a keyboard from the hub and plug another in; pull the hub out and put one back."""
    with usb_vm(BEHIND_HUB, name="t-usb-hub") as vm:
        boot(vm, hubs=1)
        out, rc = run(vm, "lsusb")
        assert re.search(r"^Bus 1 Port (\d+): ID 0409:55aa QEMU QEMU USB Hub, full-speed, class 09, driver hub$", out, re.M), out
        hub_port = re.search(r"^Bus 1 Port (\d+): ID 0409:55aa", out, re.M).group(1)
        assert re.search(rf"^Bus 1 Port {hub_port}\.1: ID 0627:0001 QEMU QEMU USB Keyboard, full-speed, .*driver hid-keyboard$", out, re.M), out
        assert re.search(rf"^Bus 1 Port {hub_port}\.2: ID 0627:0001 QEMU QEMU USB Mouse, full-speed, .*driver hid-mouse$", out, re.M), out
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        no_errors(vm)
        # unplug the keyboard from its hub port, plug another one into a different port
        vm.qmp("device_del", id="ukbd")
        wait_count(vm, GONE, 1, timeout=30)
        time.sleep(0.5)
        assert len(devices(vm)) == 2 and stat(vm)["keyboards"] == 0
        vm.qmp("device_add", driver="usb-kbd", id="ukbd3", bus="xhci.0", port="1.4")
        wait_count(vm, KBD_UP, 2, timeout=40)
        assert any(ln.startswith(f"Bus 1 Port {hub_port}.4:") and "hid-keyboard" in ln for ln in devices(vm)), devices(vm)
        type_run(vm, "echo $((7*7))\n", r"\n49\r?\n")
        # pull the hub out: the children go with it
        vm.qmp("device_del", id="hub1")
        wait_count(vm, GONE, 4, timeout=30)
        time.sleep(0.5)
        assert devices(vm) == [], devices(vm)
        s = stat(vm)
        assert s["keyboards"] == 0 and s["mice"] == 0, s
        # put a hub back with a keyboard behind it
        vm.qmp("device_add", driver="usb-hub", id="hub2", bus="xhci.0", port="2")
        vm.qmp("device_add", driver="usb-kbd", id="ukbd2", bus="xhci.0", port="2.3")
        wait_count(vm, KBD_UP, 3, timeout=40)
        lines = devices(vm)
        assert len(lines) == 2 and any("hid-keyboard" in ln and "." in ln.split(":")[0] for ln in lines), lines
        type_run(vm, "echo $((8*8))\n", r"\n64\r?\n")
        no_errors(vm)


@test
def usb_nested_hubs():
    """A hub behind a hub: route strings through two tiers, keyboard and mouse on different tiers both work."""
    extra = ["-device", "usb-hub,id=h1,bus=xhci.0,port=1", "-device", "usb-hub,id=h2,bus=xhci.0,port=1.1",
             "-device", "usb-kbd,id=ukbd,bus=xhci.0,port=1.1.2", "-device", "usb-mouse,id=umouse,bus=xhci.0,port=1.3"]
    with usb_vm(extra, name="t-usb-nested") as vm:
        boot(vm, hubs=2)
        lines = devices(vm)
        hub_port = re.search(r"^Bus 1 Port (\d+): ID 0409:55aa", "\n".join(lines), re.M).group(1)
        text = "\n".join(lines)
        assert re.search(rf"^Bus 1 Port {hub_port}\.1: ID 0409:55aa .*driver hub$", text, re.M), text
        assert re.search(rf"^Bus 1 Port {hub_port}\.1\.2: ID 0627:0001 .*USB Keyboard.*driver hid-keyboard$", text, re.M), text
        assert re.search(rf"^Bus 1 Port {hub_port}\.3: ID 0627:0001 .*USB Mouse.*driver hid-mouse$", text, re.M), text
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        vm.select_mouse("HID")
        s0 = stat(vm)
        vm.mouse_move(5, 6)
        time.sleep(0.5)
        s1 = stat(vm)
        assert s1["mouse_dx"] - s0["mouse_dx"] == 5 and s1["mouse_dy"] - s0["mouse_dy"] == 6, (s0, s1)
        no_errors(vm)
        vm.qmp("device_del", id="h2")                                   # the inner hub and the keyboard on it go away
        wait_count(vm, GONE, 2, timeout=30)
        time.sleep(0.5)
        lines = devices(vm)
        assert len(lines) == 2 and stat(vm)["keyboards"] == 0 and stat(vm)["mice"] == 1, (lines, stat(vm))


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


# ---------------------------------------------------------------- error recovery

@test
def usb_stall_recovery():
    """A STALLed control request halts the pipe; the driver clears it (Reset Endpoint + Set TR Dequeue Pointer) and carries on."""
    with usb_vm(KBD + MOUSE, cmdline="usb=stalltest", name="t-usb-stall") as vm:
        boot(vm)
        wait_count(vm, r"stall test: bogus request returned -32, device descriptor afterwards returned 18", 2, timeout=30)
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")
        assert "INCIDENT" not in vm.text()


@test
def usb_recover_control_timeout():
    """A control transfer that never completes times out, the endpoint is stopped and re-pointed, the device is enumerated again."""
    with usb_vm(KBD + MOUSE, cmdline="usb=debug usbfault=ctl:3", name="t-usb-ctlfault") as vm:
        boot(vm, timeout=90)
        text = vm.text()
        assert "control doorbell not rung" in text, text[-2000:]
        assert re.search(r"setup failed at 'configuration descriptor' \(-110\), attempt 1 of 4", text), text[-2000:]
        assert "giving up" not in text
        assert len(devices(vm)) == 2
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")


@test
def usb_recover_command_timeout():
    """A command the controller never answers times out, the command ring is aborted and restarted, the device is enumerated again."""
    with usb_vm(KBD + MOUSE, cmdline="usb=debug usbfault=cmd:4", name="t-usb-cmdfault") as vm:
        boot(vm, timeout=90)
        text = vm.text()
        assert "command doorbell not rung" in text and "command type 11 timed out" in text, text[-2000:]
        assert "command ring aborted and restarted" in text, text[-2000:]
        assert re.search(r"setup failed at 'address device' \(-110\), attempt 1 of 4", text), text[-2000:]
        assert len(devices(vm)) == 2
        type_run(vm, "echo $((6*7))\n", r"\n42\r?\n")


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
