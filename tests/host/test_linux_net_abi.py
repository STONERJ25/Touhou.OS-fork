#!/usr/bin/env python3
"""Host-side checks of the socket, poll, epoll, eventfd and timerfd facts of the Linux personality against a real Linux (docs/COMPAT.md): every constant of
kernel/linux/netabi.h is compared with the C library header of the machine this runs on (the compiler evaluates both in one program), and the size and the field
offsets of every structure with the C library's structure. The system call numbers of these calls are checked by test_linux_abi.py, like all others.

    python tools/hosttest.py discover -s tests/host -p "test_linux_net_abi.py" -v        (on Windows: runs in WSL's Ubuntu)

Needs gcc and a Linux. Nothing here reads an implementation of Linux: the headers are the interface every program is built against.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
NETABI = REPO / "kernel" / "linux" / "netabi.h"

INCLUDES = """#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
"""

# the constants of netabi.h that have no (or a differently spelled) counterpart in the C library, and what to compare them with instead
RENAMED = {"SOCK_TYPE_MASK": "0xf"}              # <linux/net.h> has it, but cannot be included beside <sys/socket.h>
OURS = {"SOCK_TYPE_MASK"}


def compile_and_run(source, extra_includes=()):
    gcc = shutil.which("gcc")
    if not gcc:
        raise unittest.SkipTest("gcc is not available")
    with tempfile.TemporaryDirectory(prefix="lnxnet-") as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        src.write_text(source, encoding="utf-8")
        subprocess.run([gcc, "-Wno-pedantic", "-I", str(REPO / "kernel" / "include"), "-I", str(REPO / "kernel" / "linux"), *extra_includes, "-o", str(exe), str(src)],
                       check=True, capture_output=True, text=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def constant_names():
    """Every '#define LNX_NAME value' of netabi.h that is a plain constant (macros with arguments are not)."""
    return re.findall(r"^#define\s+LNX_(\w+)\s+[^\s(]|^#define\s+LNX_(\w+)\s+\(", NETABI.read_text(encoding="utf-8"), re.M)


class Constants(unittest.TestCase):
    def test_every_constant_is_what_the_c_library_says(self):
        names = sorted({a or b for a, b in constant_names() if (a or b) not in ("CMSG_ALIGN", "CMSG_LEN", "CMSG_SPACE")})
        self.assertGreater(len(names), 120)
        body = ""
        for n in names:
            theirs = RENAMED.get(n, n)
            body += f'    if ((unsigned long)LNX_{n} != (unsigned long)({theirs})) {{ printf("{n}: ours %lu, the library %lu\\n", (unsigned long)LNX_{n}, (unsigned long)({theirs})); bad++; }}\n'
        out = compile_and_run(INCLUDES + '#include "netabi.h"\nint main(void) {\n    int bad = 0;\n' + body + '    printf("%d checked, %d wrong\\n", ' + str(len(names)) + ', bad);\n    return 0;\n}\n')
        self.assertTrue(out.strip().endswith(f"{len(names)} checked, 0 wrong"), out)


class Structures(unittest.TestCase):
    """size and the offset of every field, as the compiler sees them in our structure and in the library's"""

    PAIRS = [
        ("lnx_iov", "iovec", ["iov_base", "iov_len"]),
        ("lnx_msghdr", "msghdr", ["msg_name", "msg_namelen", "msg_iov", "msg_iovlen", "msg_control", "msg_controllen", "msg_flags"]),
        ("lnx_mmsghdr", "mmsghdr", ["msg_hdr", "msg_len"]),
        ("lnx_cmsghdr", "cmsghdr", ["cmsg_len", "cmsg_level", "cmsg_type"]),
        ("lnx_ucred", "ucred", ["pid", "uid", "gid"]),
        ("lnx_linger", "linger", ["l_onoff", "l_linger"]),
        ("lnx_sockaddr_un", "sockaddr_un", ["sun_family", "sun_path"]),
        ("lnx_timespec", "timespec", ["tv_sec", "tv_nsec"]),
        ("lnx_timeval", "timeval", ["tv_sec", "tv_usec"]),
        ("lnx_itimerspec", "itimerspec", ["it_interval", "it_value"]),
        ("lnx_pollfd", "pollfd", ["fd", "events", "revents"]),
        ("lnx_epoll_event", "epoll_event", ["events", "data"]),
    ]

    def test_layouts_are_the_ones_of_the_c_library(self):
        body = ""
        for ours, theirs, fields in self.PAIRS:
            body += f'    printf("{ours} size %zu\\n", sizeof(struct {ours}));\n    printf("{theirs} size %zu\\n", sizeof(struct {theirs}));\n'
            for f in fields:
                ours_field = f
                # our field names are the library's, except where a structure is shared (the timespec names carry no prefix difference)
                body += f'    printf("{ours}.{f} %zu\\n", offsetof(struct {ours}, {ours_field}));\n    printf("{theirs}.{f} %zu\\n", offsetof(struct {theirs}, {f}));\n'
        out = compile_and_run(INCLUDES + '#include "netabi.h"\nint main(void) {\n' + body + "    return 0;\n}\n")
        values = dict(l.rsplit(" ", 1) for l in out.splitlines())
        wrong = []
        for ours, theirs, fields in self.PAIRS:
            if values[f"{ours} size"] != values[f"{theirs} size"]:
                wrong.append(f"{ours}: size {values[f'{ours} size']} against {values[f'{theirs} size']}")
            for f in fields:
                if values[f"{ours}.{f}"] != values[f"{theirs}.{f}"]:
                    wrong.append(f"{ours}.{f}: offset {values[f'{ours}.{f}']} against {values[f'{theirs}.{f}']}")
        self.assertEqual(wrong, [])
        self.assertEqual(values["lnx_epoll_event size"], "12")          # packed on x86-64
        self.assertEqual(values["lnx_msghdr size"], "56")

    def test_control_message_macros(self):
        body = "".join(f'    printf("{n} %zu %zu %zu %zu\\n", LNX_CMSG_ALIGN({n}), CMSG_ALIGN({n}), LNX_CMSG_SPACE({n}), CMSG_SPACE({n}));\n    printf("len {n} %zu %zu\\n", LNX_CMSG_LEN({n}), CMSG_LEN({n}));\n'
                       for n in (0, 1, 4, 8, 12, 20, 100))
        out = compile_and_run(INCLUDES + '#include "netabi.h"\nint main(void) {\n' + body + "    return 0;\n}\n")
        for line in out.splitlines():
            parts = line.split()
            if parts[0] == "len":
                self.assertEqual(parts[2], parts[3], line)
            else:
                self.assertEqual(parts[1], parts[2], line)
                self.assertEqual(parts[3], parts[4], line)


if __name__ == "__main__":
    unittest.main()
