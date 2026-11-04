#!/usr/bin/env python3
"""Host-side checks of the Linux personality's facts against a real Linux (docs/COMPAT.md): the call numbers, the constants and the struct layouts in
kernel/linux/*.c are compared with what the C library headers and the running kernel of this machine say, and the stored outputs of the test programs
(tests/linux/golden/) are checked against what this Linux prints now.

    python tools/hosttest.py discover -s tests/host -p "test_linux_abi.py" -v        (on Windows: runs in WSL's Ubuntu)

Needs gcc and a Linux. Nothing here reads an implementation of Linux: the headers are the interface every program is built against.
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
KERNEL = REPO / "kernel" / "linux"

TYPES = ("#include <stdio.h>\n#include <stdint.h>\ntypedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef int64_t s64; typedef int32_t s32;\n"
         "typedef uint8_t u8;\n")


def compile_and_run(source, extra=()):
    gcc = shutil.which("gcc")
    if not gcc:
        raise unittest.SkipTest("gcc is not available")
    with tempfile.TemporaryDirectory(prefix="lnxabi-") as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        src.write_text(source, encoding="utf-8")
        subprocess.run([gcc, "-D_GNU_SOURCE", "-o", str(exe), str(src), *extra], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def defines(path, prefix):
    """{name without the prefix: value} for every '#define PREFIXname number' of the file"""
    out = {}
    for m in re.finditer(r"^#define\s+" + prefix + r"(\w+)\s+(0x[0-9a-fA-F]+|\d+)[uU]?\b", path.read_text(encoding="utf-8"), re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def offsets(t, fields):
    """C statements that print the offset of every field of type t and its size"""
    return "".join(f'    printf("{f} %zu\\n", __builtin_offsetof({t}, {f}));\n' for f in fields) + f'    printf("size %zu\\n", sizeof({t}));\n'


def struct_text(source, name):
    """the text of 'struct NAME { ... };' in a C source"""
    return re.search(r"struct " + name + r" \{.*?\n\};", source, re.S).group(0)


class CallNumbers(unittest.TestCase):
    def setUp(self):
        self.ours = {k: v for k, v in defines(KERNEL / "numbers.h", "LNX_").items()
                     if not k.startswith(("ARCH_", "AT_", "RLIMIT_", "RLIM_", "CLONE_", "CSIGNAL", "SA_", "SIG_", "SS_", "MINSIGSTKSZ", "FUTEX_"))}

    def test_every_number_is_what_linux_says(self):
        body = "".join(f'    printf("{n} %ld\\n", (long)SYS_{n});\n' for n in self.ours)
        out = compile_and_run("#include <stdio.h>\n#include <sys/syscall.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}
        wrong = {n: (self.ours[n], real[n]) for n in self.ours if self.ours[n] != real[n]}
        self.assertEqual(wrong, {}, "name: (ours, Linux's)")
        self.assertGreater(len(self.ours), 60)

    def test_numbers_agree_with_the_name_list(self):
        names = dict((int(m.group(1)), m.group(2)) for m in re.finditer(r'^\[(\d+)\] = "(\w+)",', (KERNEL / "names.inc").read_text(encoding="utf-8"), re.M))
        wrong = {n: (v, names.get(v)) for n, v in self.ours.items() if names.get(v) != n}
        self.assertEqual(wrong, {}, "name: (number, name the list has for it)")
        self.assertGreaterEqual(len(names), 300)
        self.assertLess(max(names), defines(KERNEL / "numbers.h", "")["LINUX_NSYSCALLS"])


class Constants(unittest.TestCase):
    def test_arch_prctl_flags_and_rlimits_are_what_linux_says(self):
        ours = defines(KERNEL / "numbers.h", "LNX_")
        pairs = [(k, "ARCH_" + k[5:]) for k in ours if k.startswith("ARCH_")]
        pairs += [(k, k) for k in ours if k.startswith("AT_")]                                       # AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH, AT_NO_AUTOMOUNT
        pairs += [(k, k) for k in ours if k.startswith(("RLIMIT_", "RLIM_NLIMITS", "CLONE_", "CSIGNAL"))]
        body = "".join(f'    printf("{k} %ld\\n", (long){c});\n' for k, c in pairs)
        out = compile_and_run("#include <stdio.h>\n#include <fcntl.h>\n#include <sched.h>\n#include <sys/resource.h>\n#include <asm/prctl.h>\nint main(void) {\n" + body +
                              "    return 0;\n}\n")
        real = {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}
        wrong = {k: (ours[k], real[k]) for k, _ in pairs if k in real and ours[k] != real[k]}
        self.assertEqual(wrong, {}, "name: (ours, Linux's)")
        self.assertEqual(len(real), len(pairs))

    def test_signal_constants_are_what_linux_says(self):
        ours = {k: v for k, v in defines(KERNEL / "numbers.h", "LNX_").items() if k.startswith(("SA_", "SIG_", "SS_", "MINSIGSTKSZ"))}
        kernel_only = ("SA_RESTORER", "MINSIGSTKSZ")                       # not in the C library's <signal.h> (its MINSIGSTKSZ is a larger, user-space number)
        libc = [k for k in ours if k not in kernel_only]
        body = "".join(f'    printf("{k} %lu\\n", (unsigned long){k});\n' for k in libc)
        out = compile_and_run("#include <stdio.h>\n#include <signal.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        body = "".join(f'#ifdef {k}\n    printf("{k} %lu\\n", (unsigned long){k});\n#endif\n' for k in kernel_only)
        out += compile_and_run("#include <stdio.h>\n#include <asm/signal.h>\nint main(void) {\n" + body + "    return 0;\n}\n")          # the kernel's own header
        real = {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}
        self.assertEqual({k: (v, real[k]) for k, v in ours.items() if k in real and real[k] != v}, {})
        self.assertIn("SA_RESTORER", real)
        self.assertGreaterEqual(len(real), 13)

    def test_futex_constants_are_what_linux_says(self):
        ours = {k: v for k, v in defines(KERNEL / "numbers.h", "LNX_").items() if k.startswith("FUTEX_") and k != "FUTEX_CMD_MASK"}     # CMD_MASK is ours: the low seven bits
        body = "".join(f'    printf("{k} %ld\\n", (long){k});\n' for k in ours)
        out = compile_and_run("#include <stdio.h>\n#include <linux/futex.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}
        self.assertEqual({k: (v, real[k]) for k, v in ours.items() if real[k] != v}, {})
        self.assertGreaterEqual(len(ours), 8)

    def test_auxiliary_vector_types_are_what_linux_says(self):
        src = (KERNEL / "stack.c").read_text(encoding="utf-8")
        ours = {m.group(1): int(m.group(2)) for m in re.finditer(r"^#define\s+(AT_\w+)\s+(\d+)\b", src, re.M)}
        body = "".join(f'    printf("{n} %ld\\n", (long){n});\n' for n in ours)
        out = compile_and_run("#include <stdio.h>\n#include <elf.h>\nint main(void) {\n" + body + "    return 0;\n}\n")
        real = {l.split()[0]: int(l.split()[1]) for l in out.splitlines()}
        self.assertEqual({n: (v, real[n]) for n, v in ours.items() if real[n] != v}, {})
        self.assertGreaterEqual(len(ours), 19)


class StructStat(unittest.TestCase):
    def test_layout_is_the_one_of_x86_64_linux(self):
        src = (KERNEL / "syscall.c").read_text(encoding="utf-8")
        text = struct_text(src, "lnx_stat")
        fields = ["st_dev", "st_ino", "st_nlink", "st_mode", "st_uid", "st_gid", "st_rdev", "st_size", "st_blksize", "st_blocks"]
        extra = lambda t, a, m, c: (f'    printf("st_atime %zu\\n", __builtin_offsetof({t}, {a}));\n    printf("st_mtime %zu\\n", __builtin_offsetof({t}, {m}));\n'
                                    f'    printf("st_ctime %zu\\n", __builtin_offsetof({t}, {c}));\n')
        ours = compile_and_run(TYPES + text + "\nint main(void) {\n" + offsets("struct lnx_stat", fields) +
                               extra("struct lnx_stat", "st_atime", "st_mtime", "st_ctime") + "    return 0;\n}\n")
        real = compile_and_run("#include <stdio.h>\n#include <sys/stat.h>\nint main(void) {\n" + offsets("struct stat", fields) +
                               extra("struct stat", "st_atim", "st_mtim", "st_ctim") + "    return 0;\n}\n")
        self.assertEqual(ours, real)


class OtherStructs(unittest.TestCase):
    """struct statx and struct sysinfo: the offsets of every field and the size are those of the C library's structures on this Linux."""

    def check(self, ours_structs, ours_name, real_includes, real_name, fields):
        ours = compile_and_run(TYPES + ours_structs + "\nint main(void) {\n" + offsets(ours_name, fields) + "    return 0;\n}\n")
        real = compile_and_run(real_includes + "#include <stdio.h>\nint main(void) {\n" + offsets(real_name, fields) + "    return 0;\n}\n")
        self.assertEqual(ours, real)

    def test_statx(self):
        src = (KERNEL / "syscall.c").read_text(encoding="utf-8")
        text = re.search(r"struct lnx_statx_ts \{[^}]*\};", src).group(0) + "\n" + struct_text(src, "lnx_statx")
        fields = ["stx_mask", "stx_blksize", "stx_attributes", "stx_nlink", "stx_uid", "stx_gid", "stx_mode", "stx_ino", "stx_size", "stx_blocks",
                  "stx_attributes_mask", "stx_atime", "stx_btime", "stx_ctime", "stx_mtime", "stx_rdev_major", "stx_rdev_minor", "stx_dev_major", "stx_dev_minor"]
        self.check(text, "struct lnx_statx", "#include <fcntl.h>\n#include <sys/stat.h>\n", "struct statx", fields)       # compile_and_run defines _GNU_SOURCE

    def test_sysinfo(self):
        text = struct_text((KERNEL / "syscall.c").read_text(encoding="utf-8"), "lnx_sysinfo")
        fields = ["uptime", "loads", "totalram", "freeram", "sharedram", "bufferram", "totalswap", "freeswap", "procs", "totalhigh", "freehigh", "mem_unit"]
        self.check(text, "struct lnx_sysinfo", "#include <sys/sysinfo.h>\n", "struct sysinfo", fields)


class SignalStructs(unittest.TestCase):
    """The pieces of the signal frame (kernel/linux/signal.c): sigcontext, ucontext, stack_t, siginfo_t, offsets and sizes as on this Linux."""

    def setUp(self):
        self.src = (KERNEL / "signal.c").read_text(encoding="utf-8")

    def test_sigcontext_is_the_one_of_the_kernel_header(self):
        fields = ["r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp", "rip", "eflags", "cs", "gs", "fs", "ss",
                  "err", "trapno", "oldmask", "cr2", "fpstate"]
        ours = compile_and_run(TYPES + struct_text(self.src, "lnx_sigcontext") + "\nint main(void) {\n" + offsets("struct lnx_sigcontext", fields) + "    return 0;\n}\n")
        real = compile_and_run("#include <stdio.h>\n#include <asm/sigcontext.h>\nint main(void) {\n" + offsets("struct sigcontext", fields) + "    return 0;\n}\n")
        self.assertEqual(ours, real)

    def test_ucontext_stack_and_siginfo_agree_with_the_c_library(self):
        structs = (re.search(r"struct lnx_stack \{[^}]*\};", self.src).group(0) + "\n" + struct_text(self.src, "lnx_sigcontext") + "\n" +
                   struct_text(self.src, "lnx_ucontext") + "\n" + re.search(r"struct lnx_siginfo \{[^}]*\};", self.src).group(0) + "\n")

        def probe(uc, stack, info, addr):
            return (offsets(uc, ["uc_flags", "uc_link", "uc_stack", "uc_mcontext", "uc_sigmask"]).replace("size ", "ucontext_size ") +
                    f'    printf("stack_t %zu\\n", sizeof({stack}));\n    printf("ss_size %zu\\n", __builtin_offsetof({stack}, ss_size));\n'
                    f'    printf("siginfo %zu\\n", sizeof({info}));\n    printf("si_code %zu\\n", __builtin_offsetof({info}, si_code));\n'
                    f'    printf("si_addr %zu\\n", __builtin_offsetof({info}, {addr}));\n')

        ours = compile_and_run(TYPES + structs + "\nint main(void) {\n" + probe("struct lnx_ucontext", "struct lnx_stack", "struct lnx_siginfo", "fields") + "    return 0;\n}\n")
        real = compile_and_run("#include <stdio.h>\n#include <signal.h>\n#include <ucontext.h>\nint main(void) {\n" +
                               probe("ucontext_t", "stack_t", "siginfo_t", "si_addr") + "    return 0;\n}\n")
        # the C library's ucontext_t is longer than the kernel's (a 128-byte signal set, extra FP state after the machine state): every other line must agree
        drop = lambda s: "\n".join(l for l in s.splitlines() if not l.startswith("ucontext_size"))
        self.assertEqual(drop(ours), drop(real))


class Oracle(unittest.TestCase):
    def test_stored_outputs_are_what_this_linux_prints(self):
        if not shutil.which("gcc"):
            self.skipTest("gcc is not available")
        r = subprocess.run([sys.executable, str(REPO / "tools" / "linux-oracle.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertGreaterEqual(r.stdout.count(": same ("), 3)


if __name__ == "__main__":
    unittest.main()
