#!/usr/bin/env python3
"""Host-side checks of the signal facts of the Linux personality against a real Linux (docs/COMPAT.md): the layout of siginfo_t as the kernel keeps it
(kernel/include/siginfo.h), the SI_* and CLD_* codes, the numbers of the signal, timer and wait calls that kernel/linux/numbers.h names, the constants of the timer
calls and the structures they pass (kernel/linux/timers.c). Like test_linux_abi.py this compiles small programs against the C library headers of this machine (the
interface every Linux program is built against) and compares; nothing here reads an implementation of Linux.

    python tools/hosttest.py discover -s tests/host -p "test_linux_signals_abi.py" -v        (on Windows: runs in WSL's Ubuntu)
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
KERNEL = REPO / "kernel"
LINUX = KERNEL / "linux"


def compile_and_run(source, extra=(), includes=()):
    gcc = shutil.which("gcc")
    if not gcc:
        raise unittest.SkipTest("gcc is not available")
    with tempfile.TemporaryDirectory(prefix="lnxsig-") as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        src.write_text(source, encoding="utf-8")
        subprocess.run([gcc, "-w", "-D_GNU_SOURCE", *[f"-I{i}" for i in includes], "-o", str(exe), str(src), *extra], check=True)       # -w: the kernel header and the C library define the same codes
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def defines(path, prefix=""):
    """{name: value} for every '#define NAME number' of the file whose name starts with prefix"""
    out = {}
    for m in re.finditer(r"^#define\s+(" + prefix + r"\w+)\s+\(?(-?0x[0-9a-fA-F]+|-?\d+)\)?[uU]?\b", path.read_text(encoding="utf-8"), re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def values(text):
    return {l.split()[0]: int(l.split()[1]) for l in text.splitlines()}


SIGINFO_PROBE = r"""
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32; typedef int64_t s64;
#define K_TYPES_H 1
#define K_STRING_H 1
#include "siginfo.h"
int main(void) {
    SigInfo k;
    siginfo_t *s = (siginfo_t *)&k;
    printf("size %zu\n", sizeof(SigInfo));
    printf("libcsize %zu\n", sizeof(siginfo_t));
    siginfo_clear(&k, SIGUSR1, SI_QUEUE);
    siginfo_sender(&k, 1234, 5678);
    siginfo_value(&k, 0x1122334455667788ULL);
    printf("signo %d\ncode %d\npid %d\nuid %u\n", s->si_signo, s->si_code, s->si_pid, (unsigned)s->si_uid);
    printf("value %llx\n", (unsigned long long)(uintptr_t)s->si_value.sival_ptr);
    siginfo_clear(&k, SIGCHLD, CLD_EXITED);
    siginfo_sender(&k, 77, 88);
    siginfo_child(&k, 9, 1000, 2000);
    printf("child_pid %d\nchild_uid %u\nstatus %d\nutime %ld\nstime %ld\n", s->si_pid, (unsigned)s->si_uid, s->si_status, (long)s->si_utime, (long)s->si_stime);
    siginfo_clear(&k, SIGRTMIN, SI_TIMER);
    siginfo_timer(&k, 5, 6, 0xabcdef);
    printf("tid %d\noverrun %d\ntimer_value %llx\n", s->si_timerid, s->si_overrun, (unsigned long long)(uintptr_t)s->si_value.sival_ptr);
    return 0;
}
"""


class SigInfo(unittest.TestCase):
    """kernel/include/siginfo.h keeps the 128-byte siginfo_t of x86-64 Linux; its accessors write the fields where the C library's siginfo_t has them."""

    def test_the_accessors_write_where_the_c_library_reads(self):
        got = {}
        for line in compile_and_run(SIGINFO_PROBE, includes=[KERNEL / "include"]).splitlines():
            k, v = line.split()
            got[k] = int(v, 16) if k in ("value", "timer_value") else int(v)
        self.assertEqual(got["size"], got["libcsize"])
        self.assertEqual((got["signo"], got["code"], got["pid"], got["uid"]), (10, -1, 1234, 5678))
        self.assertEqual(got["value"], 0x1122334455667788)
        self.assertEqual((got["child_pid"], got["child_uid"], got["status"], got["utime"], got["stime"]), (77, 88, 9, 1000, 2000))
        self.assertEqual((got["tid"], got["overrun"], got["timer_value"]), (5, 6, 0xabcdef))


class Codes(unittest.TestCase):
    def test_si_and_cld_codes_are_what_linux_says(self):
        ours = {k: v for k, v in defines(KERNEL / "include" / "siginfo.h").items() if k.startswith(("SI_", "CLD_")) and k != "SI_KERNEL_"}
        body = "".join(f'    printf("{k} %ld\\n", (long){k});\n' for k in ours)
        out = compile_and_run("#include <stdio.h>\n#include <signal.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = values(out)
        self.assertEqual({k: (v, real[k]) for k, v in ours.items() if real[k] != v}, {})
        self.assertGreaterEqual(len(ours), 12)

    def test_real_time_signal_range(self):
        # the kernel's first real-time signal is 32 (the C library takes 32 and 33 and reports SIGRTMIN as 34); the last is 64
        src = (KERNEL / "include" / "siginfo.h").read_text(encoding="utf-8")
        lo = int(re.search(r"#define SIGRTMIN_K\s+(\d+)", src).group(1))
        hi = int(re.search(r"#define SIGRTMAX_K\s+(\d+)", src).group(1))
        out = compile_and_run('#include <stdio.h>\n#include <signal.h>\nint main(void) { printf("min %d\\nmax %d\\nnsig %d\\n", SIGRTMIN, SIGRTMAX, _NSIG); return 0; }\n')
        real = values(out)
        self.assertEqual(hi, real["max"])
        self.assertEqual(lo + 2, real["min"])
        self.assertEqual(real["nsig"], hi + 1)

    def test_waitid_constants_are_what_linux_says(self):
        ours = defines(KERNEL / "include" / "process.h", "WAITID_")
        names = {"ALL": "P_ALL", "PID": "P_PID", "PGID": "P_PGID", "NOHANG": "WNOHANG", "STOPPED": "WSTOPPED", "EXITED": "WEXITED", "CONTINUED": "WCONTINUED",
                 "NOWAIT": "WNOWAIT"}
        body = "".join(f'    printf("{k} %ld\\n", (long){c});\n' for k, c in names.items())
        out = compile_and_run("#include <stdio.h>\n#include <sys/wait.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = values(out)
        self.assertEqual({k: (ours["WAITID_" + k], real[k]) for k in names if ours["WAITID_" + k] != real[k]}, {})
        self.assertEqual(len(ours), len(names))

    def test_call_numbers_of_the_signal_calls(self):
        ours = {k: v for k, v in defines(LINUX / "numbers.h", "LNX_").items()
                if k.startswith(("LNX_rt_sig", "LNX_rt_tgsig", "LNX_kill", "LNX_tkill", "LNX_tgkill", "LNX_waitid", "LNX_timer_", "LNX_clock_", "LNX_getitimer",
                                 "LNX_setitimer", "LNX_times", "LNX_getrusage", "LNX_signalfd"))}
        body = "".join(f'    printf("{k} %ld\\n", (long)SYS_{k[4:]});\n' for k in ours)
        out = compile_and_run("#include <stdio.h>\n#include <sys/syscall.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = values(out)
        self.assertEqual({k: (v, real[k]) for k, v in ours.items() if real[k] != v}, {})
        self.assertGreaterEqual(len(ours), 6)


class Timers(unittest.TestCase):
    """kernel/linux/timers.c: the numbers of the clocks, notifications and interval timers, and the structures the timer calls pass."""

    def setUp(self):
        self.src = (LINUX / "timers.c").read_text(encoding="utf-8")

    def test_clock_notification_and_itimer_numbers(self):
        pairs = {}
        for m in re.finditer(r"^#define\s+(CLK_|SIGEV_|ITIMER_)(\w+)\s+(\d+)\b", self.src, re.M):
            prefix, name, value = m.groups()
            pairs[{"CLK_": "CLOCK_", "SIGEV_": "SIGEV_", "ITIMER_": "ITIMER_"}[prefix] + name] = int(value)
        body = "".join(f'    printf("{k} %ld\\n", (long){k});\n' for k in pairs)
        out = compile_and_run("#include <stdio.h>\n#include <time.h>\n#include <signal.h>\n#include <sys/time.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = values(out)
        self.assertEqual({k: (v, real[k]) for k, v in pairs.items() if real[k] != v}, {})
        self.assertGreaterEqual(len(pairs), 18)

    def test_timer_abstime(self):
        self.assertEqual(int(re.search(r"#define LNX_TIMER_ABSTIME\s+(\d+)", self.src).group(1)), 1)
        out = compile_and_run("#include <stdio.h>\n#include <time.h>\nint main(void) { printf(\"v %d\\n\", TIMER_ABSTIME); return 0; }\n")
        self.assertEqual(values(out)["v"], 1)

    def test_structures_are_the_ones_of_the_c_library(self):
        types = ("#include <stdio.h>\n#include <stdint.h>\ntypedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef int64_t s64; typedef int32_t s32;\n")
        text = "".join(re.search(r"struct " + n + r" \{.*?\};", self.src, re.S).group(0) + "\n"
                       for n in ("lnx_timespec", "lnx_timeval", "lnx_itimerspec", "lnx_itimerval", "lnx_sigevent"))

        def probe(spec, itimerspec, itimerval, sigevent):
            return (f'    printf("itimerspec %zu %zu %zu\\n", sizeof({itimerspec}), __builtin_offsetof({itimerspec}, it_interval), __builtin_offsetof({itimerspec}, it_value));\n'
                    f'    printf("itimerval %zu %zu %zu\\n", sizeof({itimerval}), __builtin_offsetof({itimerval}, it_interval), __builtin_offsetof({itimerval}, it_value));\n'
                    f'    printf("sigevent %zu %zu %zu %zu\\n", sizeof({sigevent}), __builtin_offsetof({sigevent}, sigev_value), __builtin_offsetof({sigevent}, sigev_signo), '
                    f'__builtin_offsetof({sigevent}, sigev_notify));\n'
                    f'    printf("tid %zu\\n", __builtin_offsetof({sigevent}, {spec}));\n')

        ours = compile_and_run(types + text + "\nint main(void) {\n" +
                               probe("tid", "struct lnx_itimerspec", "struct lnx_itimerval", "struct lnx_sigevent")
                               .replace("it_interval", "interval").replace("it_value", "value").replace("sigev_value", "value").replace("sigev_signo", "signo")
                               .replace("sigev_notify", "notify") + "    return 0;\n}\n")
        real = compile_and_run("#include <stdio.h>\n#include <time.h>\n#include <signal.h>\n#include <sys/time.h>\nint main(void) {\n" +
                               probe("_sigev_un._tid", "struct itimerspec", "struct itimerval", "struct sigevent") + "    return 0;\n}\n")
        self.assertEqual(ours, real)


if __name__ == "__main__":
    unittest.main()
