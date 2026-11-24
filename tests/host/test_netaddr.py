#!/usr/bin/env python3
"""Host tests for the parts of "IPv6 in the programs" that need no machine:

  * user/libc/src/netconn.c: the family interleaving of RFC 8305 section 4 (pure), and the staggered connection attempts of section 5 driven
    over the host's real loopback sockets (the file uses only POSIX calls, so it is built here against the host's C library unchanged):
    an IPv6 address that refuses or never answers must not keep the IPv4 one from winning, and the time that costs is measured;
  * user/lib/http/src/url.c: URLs with bracketed IPv6 literals (RFC 3986 3.2.2, zones per RFC 6874), the Host header and the formatted URL
    putting the brackets back, and references resolved against such a URL.

Both files are built with gcc into a shared library (UBSan on, -Werror) and driven through ctypes; every expectation is written
here from the RFCs.

    python tools/hosttest.py discover -s tests/host -p "test_netaddr.py" -v
"""
import ctypes
import os
import shutil
import socket
import subprocess
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-netaddr"

HARNESS = r'''
#include <netconn.h>
#include <http.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* fams[i] is 4 or 6; order receives the original index of each entry after netconn_interleave */
int t_interleave(int n, const int *fams, int *order) {
    struct addrinfo ai[64];
    const struct addrinfo *v[64];
    memset(ai, 0, sizeof ai);
    for (int i = 0; i < n; i++) { ai[i].ai_family = fams[i] == 6 ? AF_INET6 : AF_INET; v[i] = &ai[i]; }
    netconn_interleave(v, n);
    for (int i = 0; i < n; i++) order[i] = (int)(v[i] - ai);
    return 0;
}

/* addrs[i] is "::1" or "127.0.0.1", ports[i] its port; returns the index that won or -1 (*err then the errno) */
int t_connect(int n, const char **addrs, const int *ports, int attempt, int stagger, int total, int *err) {
    struct addrinfo ai[16];
    struct sockaddr_storage ss[16];
    memset(ai, 0, sizeof ai);
    memset(ss, 0, sizeof ss);
    for (int i = 0; i < n; i++) {
        if (strchr(addrs[i], ':')) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss[i];
            s6->sin6_family = AF_INET6;
            s6->sin6_port = htons((unsigned short)ports[i]);
            inet_pton(AF_INET6, addrs[i], &s6->sin6_addr);
            ai[i].ai_family = AF_INET6;
            ai[i].ai_addrlen = sizeof *s6;
        } else {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&ss[i];
            s4->sin_family = AF_INET;
            s4->sin_port = htons((unsigned short)ports[i]);
            inet_pton(AF_INET, addrs[i], &s4->sin_addr);
            ai[i].ai_family = AF_INET;
            ai[i].ai_addrlen = sizeof *s4;
        }
        ai[i].ai_socktype = SOCK_STREAM;
        ai[i].ai_addr = (struct sockaddr *)&ss[i];
        ai[i].ai_next = i + 1 < n ? &ai[i + 1] : NULL;
    }
    const struct addrinfo *won = NULL;
    int fd = netconn_connect(ai, attempt, stagger, total, err, &won);
    if (fd < 0) return -1;
    close(fd);
    return (int)(won - ai);
}

/* parses s; fills host, port, path, the formatted URL and the Host header line of a GET; returns http_url_parse's result */
int t_url(const char *s, char *host, int *port, char *path, char *formatted, char *hosthdr) {
    HttpUrl u;
    int rc = http_url_parse(s, &u);
    if (rc) return rc;
    strcpy(host, u.host);
    *port = u.port;
    strcpy(path, u.path);
    http_url_format(&u, formatted, 600);
    char req[2048];
    http_request_build(req, sizeof req, "GET", &u, NULL, 0);
    char *h = strstr(req, "\r\nHost: ");
    hosthdr[0] = 0;
    if (h) { h += 8; char *e = strstr(h, "\r\n"); size_t k = (size_t)(e - h); memcpy(hosthdr, h, k); hosthdr[k] = 0; }
    return 0;
}

int t_resolve(const char *base, const char *ref, char *formatted) {
    HttpUrl b, o;
    if (http_url_parse(base, &b)) return -100;
    int rc = http_url_resolve(&b, ref, &o);
    if (rc) return rc;
    http_url_format(&o, formatted, 600);
    return 0;
}
'''


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    inc = OUT / "inc"                                   # only the project headers these files need: the host's C library stays the C library
    inc.mkdir(exist_ok=True)
    for h in (REPO / "user/libc/include/netconn.h", REPO / "user/lib/http/include/http.h", REPO / "user/lib/tls/include/tls.h"):
        (inc / h.name).write_bytes(h.read_bytes())
    harness = OUT / "harness.c"
    harness.write_text(HARNESS)
    so = OUT / "netaddr.so"
    srcs = [REPO / "user/libc/src/netconn.c", REPO / "user/lib/http/src/url.c", harness]
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise unittest.SkipTest("no C compiler")
    cmd = [cc, "-shared", "-fPIC", "-O1", "-g", "-std=gnu17", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(inc), *map(str, srcs), "-o", str(so)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build failed:\n" + r.stdout + r.stderr)
    return so


LIB = None


def lib():
    global LIB
    if LIB is None:
        LIB = ctypes.CDLL(str(build()))
    return LIB


def interleave(fams):
    n = len(fams)
    arr = (ctypes.c_int * n)(*fams)
    out = (ctypes.c_int * n)()
    lib().t_interleave(n, arr, out)
    return list(out)


def expected_interleave(fams):
    """RFC 8305 section 4 with a First Address Family Count of 1: the first entry's family, then alternate, each family in its own order."""
    first = [i for i, f in enumerate(fams) if f == fams[0]]
    other = [i for i, f in enumerate(fams) if f != fams[0]]
    out = []
    while first or other:
        if first:
            out.append(first.pop(0))
        if other:
            out.append(other.pop(0))
    return out


class Interleave(unittest.TestCase):
    def test_examples(self):
        self.assertEqual(interleave([6, 6, 6, 4, 4]), [0, 3, 1, 4, 2])
        self.assertEqual(interleave([4, 6, 6]), [0, 1, 2])
        self.assertEqual(interleave([4, 4, 4, 6]), [0, 3, 1, 2])
        self.assertEqual(interleave([6, 6]), [0, 1])
        self.assertEqual(interleave([6]), [0])

    def test_every_list_up_to_eight(self):
        for n in range(1, 9):
            for bits in range(1 << n):
                fams = [6 if bits >> i & 1 else 4 for i in range(n)]
                self.assertEqual(interleave(fams), expected_interleave(fams), fams)


def have_v6_loopback():
    try:
        s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        s.bind(("::1", 0))
        s.close()
        return True
    except OSError:
        return False


def listener(family, host):
    s = socket.socket(family, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, 0))
    s.listen(8)
    return s, s.getsockname()[1]


def closed_port(family, host):
    s = socket.socket(family, socket.SOCK_STREAM)
    s.bind((host, 0))
    port = s.getsockname()[1]
    s.close()
    return port


def black_hole(family, host):
    """A port whose SYNs go unanswered: a listener with a backlog of 0 whose queue is filled and never accepted (Linux drops further SYNs)."""
    s = socket.socket(family, socket.SOCK_STREAM)
    s.bind((host, 0))
    s.listen(0)
    port = s.getsockname()[1]
    fillers = []
    for _ in range(3):
        c = socket.socket(family, socket.SOCK_STREAM)
        c.setblocking(False)
        try:
            c.connect((host, port))
        except BlockingIOError:
            pass
        fillers.append(c)
    time.sleep(0.2)
    probe = socket.socket(family, socket.SOCK_STREAM)
    probe.settimeout(0.5)
    try:
        probe.connect((host, port))
        ok = False                                      # the kernel answered after all: no black hole on this host
    except (socket.timeout, TimeoutError):
        ok = True
    except OSError:
        ok = False
    probe.close()
    return (s, fillers, port) if ok else None


def connect(entries, attempt=2000, stagger=250, total=0):
    n = len(entries)
    addrs = (ctypes.c_char_p * n)(*[a.encode() for a, _ in entries])
    ports = (ctypes.c_int * n)(*[p for _, p in entries])
    err = ctypes.c_int(0)
    f = lib().t_connect                                 # built before the clock starts
    t = time.monotonic()
    won = f(n, addrs, ports, attempt, stagger, total, ctypes.byref(err))
    return won, err.value, time.monotonic() - t


@unittest.skipUnless(have_v6_loopback(), "no IPv6 loopback on this host")
class Fallback(unittest.TestCase):
    def setUp(self):
        self.keep = []

    def tearDown(self):
        for s in self.keep:
            s.close()

    def test_first_address_wins_when_it_answers(self):
        s6, p6 = listener(socket.AF_INET6, "::1")
        s4, p4 = listener(socket.AF_INET, "127.0.0.1")
        self.keep += [s6, s4]
        won, _, dt = connect([("::1", p6), ("127.0.0.1", p4)])
        self.assertEqual(won, 0)
        self.assertLess(dt, 0.2)

    def test_refused_ipv6_goes_on_to_ipv4_at_once(self):
        s4, p4 = listener(socket.AF_INET, "127.0.0.1")
        self.keep.append(s4)
        won, _, dt = connect([("::1", closed_port(socket.AF_INET6, "::1")), ("::1", closed_port(socket.AF_INET6, "::1")), ("127.0.0.1", p4)])
        self.assertEqual(won, 2)                         # interleaved: ::1, 127.0.0.1, ::1 -> the IPv4 one is the second attempt
        self.assertLess(dt, 0.2)                         # a refusal starts the next attempt without waiting for the stagger

    def test_everything_refused(self):
        won, err, dt = connect([("::1", closed_port(socket.AF_INET6, "::1")), ("127.0.0.1", closed_port(socket.AF_INET, "127.0.0.1"))])
        self.assertEqual(won, -1)
        self.assertEqual(err, 111)                       # ECONNREFUSED, not a timeout
        self.assertLess(dt, 0.2)

    def test_black_hole_ipv6_costs_the_stagger_only(self):
        bh = black_hole(socket.AF_INET6, "::1")
        if not bh:
            self.skipTest("this host answers SYNs to a full accept queue")
        self.keep += [bh[0]] + bh[1]
        s4, p4 = listener(socket.AF_INET, "127.0.0.1")
        self.keep.append(s4)
        won, _, dt = connect([("::1", bh[2]), ("127.0.0.1", p4)])
        self.assertEqual(won, 1)
        self.assertGreaterEqual(dt, 0.2)
        self.assertLess(dt, 0.6)                         # 250 ms of head start, then the IPv4 attempt answers at once
        print(f"\n  fallback past an unanswering IPv6 address: {dt * 1000:.0f} ms")

    def test_black_hole_everywhere_times_out_per_attempt(self):
        bh = black_hole(socket.AF_INET6, "::1")
        if not bh:
            self.skipTest("this host answers SYNs to a full accept queue")
        self.keep += [bh[0]] + bh[1]
        won, err, dt = connect([("::1", bh[2]), ("::1", bh[2])], attempt=400, stagger=100, total=0)
        self.assertEqual(won, -1)
        self.assertEqual(err, 110)                       # ETIMEDOUT
        self.assertGreaterEqual(dt, 0.45)                # the second attempt starts at 100 ms and has 400 ms
        self.assertLess(dt, 0.9)
        won, err, dt = connect([("::1", bh[2]), ("::1", bh[2])], attempt=400, stagger=100, total=1200)
        self.assertEqual(won, -1)                        # with a total limit the last attempts may use it all
        self.assertGreaterEqual(dt, 1.1)
        self.assertLess(dt, 1.6)


def url(s):
    host, path, fmt, hh = (ctypes.create_string_buffer(700) for _ in range(4))
    port = ctypes.c_int(0)
    rc = lib().t_url(s.encode(), host, ctypes.byref(port), path, fmt, hh)
    if rc:
        return None
    return host.value.decode(), port.value, path.value.decode(), fmt.value.decode(), hh.value.decode()


class Urls(unittest.TestCase):
    def test_ipv6_literals(self):
        self.assertEqual(url("http://[fec0::2]:8080/x?y"), ("fec0::2", 8080, "/x?y", "http://[fec0::2]:8080/x?y", "[fec0::2]:8080"))
        self.assertEqual(url("http://[::1]/"), ("::1", 80, "/", "http://[::1]/", "[::1]"))
        self.assertEqual(url("https://[2001:DB8::A]"), ("2001:db8::a", 443, "/", "https://[2001:db8::a]/", "[2001:db8::a]"))
        self.assertEqual(url("http://[::ffff:10.0.2.2]:81/"), ("::ffff:10.0.2.2", 81, "/", "http://[::ffff:10.0.2.2]:81/", "[::ffff:10.0.2.2]:81"))
        self.assertEqual(url("http://[fe80::1%25eth0]:8080/")[0], "fe80::1%eth0")

    def test_bad_literals(self):
        for bad in ["http://[fec0::2/", "http://[fec0::2]x/", "http://[]/", "http://[1.2.3.4]/", "http://[fec0::g]/", "http://[::1]:/x:y",
                    "http://[::1]:99999/", "http://[fe80::1%eth0]/", "http://[fe80::1%25]/", "http://fec0::2/"]:
            r = url(bad)
            if bad == "http://[::1]:/x:y":                   # an empty port is the default one (RFC 3986 3.2.3)
                self.assertEqual(r[1], 80)
                continue
            self.assertIsNone(r, bad)

    def test_names_and_ipv4_unchanged(self):
        self.assertEqual(url("http://Example.COM:8080/a b"), ("example.com", 8080, "/a%20b", "http://example.com:8080/a%20b", "example.com:8080"))
        self.assertEqual(url("http://10.0.2.2/"), ("10.0.2.2", 80, "/", "http://10.0.2.2/", "10.0.2.2"))

    def test_references_against_a_literal(self):
        out = ctypes.create_string_buffer(700)
        self.assertEqual(lib().t_resolve(b"http://[fec0::2]:8080/a/b", b"c", out), 0)
        self.assertEqual(out.value.decode(), "http://[fec0::2]:8080/a/c")
        self.assertEqual(lib().t_resolve(b"http://[fec0::2]:8080/a/b", b"//[::1]:9/z", out), 0)
        self.assertEqual(out.value.decode(), "http://[::1]:9/z")
        self.assertEqual(lib().t_resolve(b"http://10.0.2.2/a", b"http://[::1]/q", out), 0)
        self.assertEqual(out.value.decode(), "http://[::1]/q")


if __name__ == "__main__":
    unittest.main()
