"""Time: POSIX time zones in libc, settable system clock, the clock chip (hwclock)."""
import test_shell
from harness import test, Qemu

US = "EST5EDT,M3.2.0,M11.1.0"
SYDNEY = "AEST-10AEDT,M10.1.0,M4.1.0/3"

# (command, substring that must appear in the output, expected status)
ZONE_CHECKS = [
    ("date -u -d @0 +%F-%T", "1970-01-01-00:00:00", 0),
    # a fixed offset east of Greenwich
    ("TZ=JST-9 date -d @0 '+%H:%M:%S %Z %z'", "09:00:00 JST +0900", 0),
    # the United States: summer and winter
    (f"TZ={US} date -d @1782907200 '+%H:%M %Z %z'", "08:00 EDT -0400", 0),
    (f"TZ={US} date -d @1796126400 '+%H:%M %Z %z'", "07:00 EST -0500", 0),
    # the second Sunday of March at 02:00 and the first Sunday of November at 02:00
    (f"TZ={US} date -d @1772953199 '+%d %H:%M:%S %Z'", "08 01:59:59 EST", 0),
    (f"TZ={US} date -d @1772953200 '+%d %H:%M:%S %Z'", "08 03:00:00 EDT", 0),
    (f"TZ={US} date -d @1793512799 '+%d %H:%M:%S %Z'", "01 01:59:59 EDT", 0),
    (f"TZ={US} date -d @1793512800 '+%d %H:%M:%S %Z'", "01 01:00:00 EST", 0),
    # the southern hemisphere: summer wraps around the new year
    (f"TZ='{SYDNEY}' date -d @1768435200 '+%H:%M %Z'", "11:00 AEDT", 0),
    (f"TZ='{SYDNEY}' date -d @1784073600 '+%H:%M %Z'", "10:00 AEST", 0),
    # parsing a local time and printing its epoch (mktime is the inverse of localtime)
    (f"TZ={US} date -d '2026-07-01 12:00:00' +%s", "1782921600", 0),
    ("TZ=JST-9 date -d '2026-07-01 12:00:00' +%s", "1782874800", 0),
    # a malformed zone falls back to UTC instead of nonsense
    ("TZ=nonsense date -u -d @0 +%H", "00", 0),
    # file timestamps follow the zone
    ("touch /tmp/zone.txt; TZ=JST-9 stat /tmp/zone.txt | grep Modify", "+0900", 0),
]

CLOCK_CHECKS = [
    # setting the clock needs root
    ("date -s '2030-01-02 03:04:05'", "cannot set the date", 1),
    ("su marisa -c \"sudo date -u -s '2030-01-02 03:04:05'\"", "Wed Jan  2 03:04:05 UTC 2030", 0),
    ("date -u +%F-%H:%M", "2030-01-02-03:04", 0),
    # the clock chip keeps UTC unless told otherwise ...
    ("su marisa -c 'sudo hwclock -w'", "", 0),
    ("cat /proc/rtc", "18935534", 0),                       # 2030-01-02 03:04:xx
    # ... and can keep local time for a PC that also runs Windows
    ("su marisa -c 'TZ=JST-9 sudo hwclock -w --localtime'", "", 0),
    ("cat /proc/rtc", "18935858", 0),                       # 2030-01-02 12:04:xx read as if it were UTC
    ("TZ=JST-9 hwclock --localtime | cut -c1-16", "2030-01-02 12:04", 0),
    # reading the chip back as local time puts the system clock right again
    ("su marisa -c \"sudo date -u -s '2001-01-01 00:00:00'\"; su marisa -c 'TZ=JST-9 sudo hwclock -s --localtime'; date -u +%F-%H", "2030-01-02-03", 0),
]


@test
def time_zones():
    """POSIX TZ strings: fixed offsets, US and southern-hemisphere DST rules, exact transition instants, mktime inverse."""
    failures = []
    with Qemu(cmdline="text", name="t-zones") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        for cmd, expect, status in ZONE_CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                if status is not None:
                    assert rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-500:]}")
    assert not failures, f"{len(failures)} zone check(s) failed:\n" + "\n".join(failures)


@test
def time_clock_chip():
    """date -s and hwclock: the system clock is settable by root, and the clock chip can hold UTC or local time."""
    failures = []
    with Qemu(cmdline="text", name="t-clockchip") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        for cmd, expect, status in CLOCK_CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                if status is not None:
                    assert rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-500:]}")
    assert not failures, f"{len(failures)} clock check(s) failed:\n" + "\n".join(failures)


@test
def time_cmdline_zone():
    """tz= on the kernel command line sets $TZ for everyone; rtc=local makes init correct the system clock."""
    with Qemu(cmdline="text tz=JST-9", name="t-tzcmdline") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, _ = test_shell.run(vm, "echo $TZ; date +%Z")
        assert "JST-9\nJST" in out, out
    # QEMU's clock chip holds UTC; telling init it is local time shifts the system clock by the zone's offset
    with Qemu(cmdline="text tz=JST-9 rtc=local", name="t-rtclocal") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, _ = test_shell.run(vm, "cat /proc/rtc; date +%s")
        chip, now = [int(x) for x in out.split()[:2]]
        assert 32000 <= chip - now <= 32800, (chip, now)      # the system clock is 9 hours behind the chip's calendar fields
