"""TLS: libtls and HTTPS in fetch and wget.

Two kinds of tests. The ones named tls_host_* never boot QEMU: the same C sources that go into libtls.a are compiled into a DLL
for the build machine (tools/tlshost.py) and checked against Python's `cryptography` package and against OpenSSL (Python's ssl
module is a TLS server here), including a proxy that corrupts or cuts the traffic. The ones named tls_guest_* boot the system
and run fetch and wget against those same servers through QEMU's user-mode network (the host is 10.0.2.2 for the guest), so they
prove that the operating system underneath (sockets, clock, random numbers, files) does what libtls needs.

The guest clock can run slower than wall time: nothing here sleeps and then asserts, every wait is for output."""
import datetime
import os
import re
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time

from harness import test

try:
    import cryptography  # noqa: F401
    import tlslib as T
    import tlskat
    HAVE_CRYPTO = True
except ImportError:                                                    # pragma: no cover
    HAVE_CRYPTO = False

def _find_openssl():
    # the MSYS build that ships with Git for Windows can be driven through pipes; the native mingw one cannot
    for cand in (r"C:\Program Files\Git\usr\bin\openssl.exe", shutil.which("openssl")):
        if cand and os.path.exists(cand):
            return cand
    return None


OPENSSL = _find_openssl()


def skip_without_crypto(name):
    if not HAVE_CRYPTO:
        print(f"SKIP {name}: needs the Python package 'cryptography' (pip install cryptography)")
        return True
    return False


# ------------------------------------------------------------------------------------------------------------ primitives

@test
def tls_host_kat_symmetric():
    """SHA-2, HMAC, HKDF, ChaCha20-Poly1305, AES (AES-NI and portable) and GCM against hashlib and cryptography, including bad tags"""
    if skip_without_crypto("tls_host_kat_symmetric"):
        return
    problems = tlskat.check_hashes() + tlskat.check_chacha_poly()
    bad, modes = tlskat.check_aes_gcm()
    problems += bad
    assert "software" in modes, modes
    assert not problems, "\n".join(problems[:20])


@test
def tls_host_kat_public_key():
    """big numbers, X25519 (RFC 7748 vectors, low-order points), P-256/P-384 ECDH and ECDSA, RSA PKCS#1 v1.5 and PSS against cryptography; malformed input is refused"""
    if skip_without_crypto("tls_host_kat_public_key"):
        return
    problems = tlskat.check_bignum() + tlskat.check_x25519() + tlskat.check_ec() + tlskat.check_rsa()
    assert not problems, "\n".join(problems[:20])


# ------------------------------------------------------------------------------------------------------------ handshakes

def pki(ca_kind="ec256", leaf_kind="ec256", sans=("localhost",), **kw):
    ca = T.issue("Test Root CA", T.new_key(ca_kind), ca=True, path_len=1)
    leaf = T.issue("localhost", T.new_key(leaf_kind), issuer=ca, sans=list(sans) if sans else None, **kw)
    return ca, leaf


def want(rc, name):
    assert rc == T.E[name], f"expected {name}, got {T.ERRORS.get(rc, rc)}"


@test
def tls_host_handshakes():
    """TLS 1.3 and 1.2 against OpenSSL: every cipher suite (AES-NI and portable AES), EC and RSA certificates, P-256/P-384 key exchange, HelloRetryRequest, ALPN, 3 MB both ways"""
    if skip_without_crypto("tls_host_handshakes"):
        return
    T.reset()
    ca, leaf = pki()
    ctx = T.Ctx(ca.pem)
    with T.Server(T.server_context(leaf)) as srv:
        for mask, suite in ((T.TLS_S_AES128, "TLS_AES_128_GCM_SHA256"), (T.TLS_S_AES256, "TLS_AES_256_GCM_SHA384"), (T.TLS_S_CHACHA, "TLS_CHACHA20_POLY1305_SHA256")):
            for flags in (0, T.TLS_F_NO_AESNI):
                rc, data, c = T.fetch(srv, ctx=ctx, suites=mask, flags=flags | T.TLS_F_TLS13_ONLY)
                assert rc == 0, (suite, T.ERRORS.get(rc), c.alert())
                assert c.info.suite_name.decode() == suite and c.info.version == 0x0304 and b"hello from OpenSSL" in data
                c.close()
        rc, data, c = T.fetch(srv, ctx=ctx, flags=T.TLS_F_TLS12_ONLY)           # a 1.3-capable server must not trigger the downgrade alarm
        assert rc == 0 and c.info.version == 0x0303, (T.ERRORS.get(rc), c.alert())
        c.close()
    for ck, lk in (("rsa2048", "rsa2048"), ("rsa2048", "ec256"), ("ec256", "rsa2048"), ("ec384", "ec384"), ("ec384", "ec256")):
        ca, leaf = pki(ck, lk)
        with T.Server(T.server_context(leaf)) as srv:
            rc, data, c = T.fetch(srv, ctx=T.Ctx(ca.pem))
            assert rc == 0 and c.info.verified == 1, (ck, lk, T.ERRORS.get(rc), c.alert())
            c.close()
    combos = [("ec256", "ECDHE-ECDSA-AES128-GCM-SHA256"), ("ec256", "ECDHE-ECDSA-AES256-GCM-SHA384"), ("ec256", "ECDHE-ECDSA-CHACHA20-POLY1305"),
              ("rsa2048", "ECDHE-RSA-AES128-GCM-SHA256"), ("rsa2048", "ECDHE-RSA-AES256-GCM-SHA384"), ("rsa2048", "ECDHE-RSA-CHACHA20-POLY1305")]
    for key, cipher in combos:
        ca, leaf = pki(key, key)
        with T.Server(T.server_context(leaf, max_version=ssl.TLSVersion.TLSv1_2, ciphers=cipher)) as srv:
            for flags in (0, T.TLS_F_NO_AESNI):
                rc, data, c = T.fetch(srv, ctx=T.Ctx(ca.pem), flags=flags)
                assert rc == 0 and c.info.version == 0x0303 and b"hello" in data, (cipher, T.ERRORS.get(rc), c.alert(), srv.errors)
                c.close()
    ca, leaf = pki()
    ctx = T.Ctx(ca.pem)
    for curve, group in (("secp384r1", "secp384r1"), ("prime256v1", "secp256r1")):
        with T.Server(T.server_context(leaf, curve=curve)) as srv:                    # TLS 1.3: the server asks for another group
            rc, data, c = T.fetch(srv, ctx=ctx)
            assert rc == 0 and c.info.hello_retry == 1 and c.info.group_name.decode() == group, (curve, T.ERRORS.get(rc), srv.errors)
            c.close()
        with T.Server(T.server_context(leaf, max_version=ssl.TLSVersion.TLSv1_2, curve=curve)) as srv:    # TLS 1.2: ServerKeyExchange names it
            rc, data, c = T.fetch(srv, ctx=ctx)
            assert rc == 0 and c.info.group_name.decode() == group, (curve, T.ERRORS.get(rc), srv.errors)
            c.close()
    for kw in ({}, {"max_version": ssl.TLSVersion.TLSv1_2}):
        with T.Server(T.server_context(leaf, alpn=["h2", "http/1.1"], **kw)) as srv:
            rc, data, c = T.fetch(srv, ctx=ctx, alpn="http/1.1")
            assert rc == 0 and c.info.alpn == b"http/1.1", (T.ERRORS.get(rc), c.info.alpn)
            c.close()
    payload = os.urandom(3 * 1024 * 1024 + 123)
    for kw in ({}, {"max_version": ssl.TLSVersion.TLSv1_2}):
        with T.Server(T.server_context(leaf, **kw), handler=T.echo_handler) as srv:
            c = T.connect(srv, ctx=ctx)
            assert c.handshake() == 0
            got = bytearray()

            def reader():
                while len(got) < len(payload):
                    r = c.read(70000)
                    if isinstance(r, int) or not r:
                        break
                    got.extend(r)
            th = threading.Thread(target=reader)
            th.start()
            for off in range(0, len(payload), 50000):
                part = payload[off:off + 50000]
                assert c.write(part) == len(part)
            th.join(60)
            assert bytes(got) == payload, (len(got), len(payload))
            c.close()


@test
def tls_host_key_update():
    """TLS 1.3 KeyUpdate in both directions (openssl s_server): the keys change under a running connection"""
    if skip_without_crypto("tls_host_key_update"):
        return
    if not OPENSSL:
        print("SKIP tls_host_key_update: no openssl command")
        return
    T.reset()
    ca = T.issue("KU CA", T.new_key("ec256"), ca=True)
    leaf = T.issue("localhost", T.new_key("ec256"), issuer=ca, sans=["localhost"])
    d = tempfile.mkdtemp(prefix="tlsku-")
    for name, text in (("leaf.pem", leaf.pem), ("leaf.key", leaf.key_pem)):
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    s0 = socket.socket()
    s0.bind(("127.0.0.1", 0))
    port = s0.getsockname()[1]
    s0.close()
    p = subprocess.Popen([OPENSSL, "s_server", "-accept", "127.0.0.1:" + str(port), "-cert", os.path.join(d, "leaf.pem"), "-key", os.path.join(d, "leaf.key"), "-tls1_3"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = bytearray()

    def pump():
        while True:
            b = p.stdout.read1(4096)
            if not b:
                break
            out.extend(b)
    threading.Thread(target=pump, daemon=True).start()
    try:
        for _ in range(50):                                            # wait for the listener
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise AssertionError("openssl s_server did not start")
        c = T.Client(sock, server_name="localhost", ctx=T.Ctx(ca.pem))
        rc = c.handshake()
        if rc != 0 and "mingw" in OPENSSL.lower():
            print("SKIP tls_host_key_update: this openssl build cannot be driven through a pipe")
            return
        assert rc == 0 and c.info.version == 0x0304, (T.ERRORS.get(rc), c.where())

        def say(text):
            p.stdin.write(text.encode())
            p.stdin.flush()

        def hear(expect):
            got = b""
            while expect not in got:
                r = c.read()
                assert isinstance(r, bytes) and r, (T.ERRORS.get(r, r), got)
                got += r
        say("before\n")
        hear(b"before\n")
        say("K\n")                                                      # the server rolls its sending key
        time.sleep(0.5)                                                 # s_server takes a whole stdin chunk as one command
        say("after one\n")
        hear(b"after one\n")
        say("k\n")                                                      # ... and asks us to roll ours as well
        time.sleep(0.5)
        say("after two\n")
        hear(b"after two\n")
        assert c.write(b"client after update\n") > 0
        for _ in range(100):
            if b"client after update" in bytes(out):
                break
            time.sleep(0.1)
        assert b"client after update" in bytes(out), bytes(out)[-300:]
        c.close()
    finally:
        p.kill()


@test
def tls_host_certificates():
    """chain validation: wrong name, expired, not yet valid, unknown CA, self-signed, wrong key usage, intermediates, path length, CA flag, pinned certificate; -k style continuation"""
    if skip_without_crypto("tls_host_certificates"):
        return
    T.reset()
    now = T.now_utc()
    day = datetime.timedelta(days=1)
    ca, leaf = pki()
    ctx = T.Ctx(ca.pem)
    other_ca = T.issue("Other CA", T.new_key("ec256"), ca=True)
    scenarios = {
        "CERT_NAME": T.issue("localhost", T.new_key("ec256"), issuer=ca, sans=["other.example"]),
        "CERT_EXPIRED": T.issue("localhost", T.new_key("ec256"), issuer=ca, sans=["localhost"], not_before=now - 10 * day, not_after=now - day),
        "CERT_NOT_YET_VALID": T.issue("localhost", T.new_key("ec256"), issuer=ca, sans=["localhost"], not_before=now + day, not_after=now + 5 * day),
        "CERT_UNTRUSTED": T.issue("localhost", T.new_key("ec256"), issuer=other_ca, sans=["localhost"]),
        "CERT_SELF_SIGNED": T.issue("localhost", T.new_key("ec256"), sans=["localhost"]),
        "CERT_USAGE": T.issue("localhost", T.new_key("ec256"), issuer=ca, sans=["localhost"], eku="client"),
    }
    for expected, cert in scenarios.items():
        for flags in (0, T.TLS_F_TLS12_ONLY):
            with T.Server(T.server_context(cert)) as srv:
                rc, data, c = T.fetch(srv, ctx=ctx, flags=flags)
                want(rc, expected)
                c.close()
        with T.Server(T.server_context(cert)) as srv:                  # with TLS_F_NO_VERIFY the connection works and the reason is kept
            rc, data, c = T.fetch(srv, ctx=ctx, flags=T.TLS_F_NO_VERIFY)
            assert rc == 0 and c.info.verified == 0 and c.info.verify_error == T.E[expected] and b"hello" in data, (expected, rc, c.info.verify_error)
            c.close()
    # no trust store at all: nothing is trusted
    with T.Server(T.server_context(leaf)) as srv:
        rc, data, c = T.fetch(srv, ctx=None)
        want(rc, "CERT_UNTRUSTED")
        c.close()
    # intermediates
    root = T.issue("Root", T.new_key("ec256"), ca=True, path_len=1)
    inter = T.issue("Intermediate", T.new_key("ec256"), issuer=root, ca=True, path_len=0)
    leaf2 = T.issue("localhost", T.new_key("ec256"), issuer=inter, sans=["localhost"])
    rctx = T.Ctx(root.pem)
    with T.Server(T.server_context(leaf2, chain=[inter])) as srv:
        rc, data, c = T.fetch(srv, ctx=rctx)
        assert rc == 0 and c.info.chain_length == 2, T.ERRORS.get(rc)
        c.close()
    with T.Server(T.server_context(leaf2)) as srv:                     # the server forgot to send the intermediate
        rc, data, c = T.fetch(srv, ctx=rctx)
        want(rc, "CERT_UNTRUSTED")
        c.close()
    junk = T.issue("Junk", T.new_key("ec256"), ca=True)
    with T.Server(T.server_context(leaf2, chain=[junk, inter])) as srv:   # extra, unrelated and out-of-order certificates do no harm
        rc, data, c = T.fetch(srv, ctx=rctx)
        assert rc == 0, T.ERRORS.get(rc)
        c.close()
    inter2 = T.issue("Intermediate2", T.new_key("ec256"), issuer=inter, ca=True, path_len=0)
    leaf3 = T.issue("localhost", T.new_key("ec256"), issuer=inter2, sans=["localhost"])
    with T.Server(T.server_context(leaf3, chain=[inter2, inter])) as srv:   # pathLenConstraint 0 forbids a second intermediate
        rc, data, c = T.fetch(srv, ctx=rctx)
        want(rc, "CERT_USAGE")
        c.close()
    notca = T.issue("NotACA", T.new_key("ec256"), issuer=root, ca=False)
    leaf4 = T.issue("localhost", T.new_key("ec256"), issuer=notca, sans=["localhost"])
    with T.Server(T.server_context(leaf4, chain=[notca])) as srv:       # an intermediate without the CA flag
        rc, data, c = T.fetch(srv, ctx=rctx)
        assert rc in (T.E["CERT_USAGE"], T.E["CERT_UNTRUSTED"]), T.ERRORS.get(rc)
        c.close()
    # a certificate the user trusts by name, with nothing above it (fetch --cacert server.pem)
    selfie = T.issue("localhost", T.new_key("ec256"), sans=["localhost"], ca=False)
    with T.Server(T.server_context(selfie)) as srv:
        rc, data, c = T.fetch(srv, ctx=T.Ctx(selfie.pem))
        assert rc == 0 and c.info.verified == 1, T.ERRORS.get(rc)
        c.close()
    ipc = T.issue("ip", T.new_key("ec256"), issuer=ca, sans=["127.0.0.1"])
    with T.Server(T.server_context(ipc)) as srv:                        # IP address in subjectAltName
        rc, data, c = T.fetch(srv, ctx=ctx, server_name="127.0.0.1")
        assert rc == 0, T.ERRORS.get(rc)
        c.close()
        rc, data, c = T.fetch(srv, ctx=ctx, server_name="127.0.0.2")
        want(rc, "CERT_NAME")
        c.close()


@test
def tls_host_names():
    """host names: wildcards only as a whole left-most label, case-insensitive, trailing dot, IP addresses only against IP entries, no common-name fallback"""
    if skip_without_crypto("tls_host_names"):
        return
    T.reset()
    ca = T.issue("Names CA", T.new_key("ec256"), ca=True)
    L = T.lib()

    def ok(sans, host):
        cert = T.issue("whatever", T.new_key("ec256"), issuer=ca, sans=sans)
        return L.tls_x509_match_name(cert.der, len(cert.der), host.encode()) == 0
    good = [(["example.com"], "example.com"), (["example.com"], "EXAMPLE.com"), (["example.com"], "example.com."), (["*.example.com"], "a.example.com"),
            (["*.example.com"], "A.EXAMPLE.COM"), (["*.example.com"], "xa-b.example.com"), (["a.example", "10.1.2.3"], "10.1.2.3"), (["10.0.2.2"], "10.0.2.2"),
            (["a.example", "*.b.example"], "a.example"), (["a.example", "*.b.example"], "q.b.example")]
    bad = [(["example.com"], "www.example.com"), (["example.com"], "xexample.com"), (["example.com"], "example.com.evil.org"), (["*.example.com"], "example.com"),
           (["*.example.com"], "a.b.example.com"), (["*.example.com"], ".example.com"), (["*.com"], "x.com"), (["*"], "x"), (["f*.example.org"], "foo.example.org"),
           (["*.*.example.net"], "a.b.example.net"), (["a.*.example.org"], "a.x.example.org"), (["10.0.2.2"], "10.0.2.3"), (["10.0.2.2"], "example.com"),
           (["example.com"], "10.0.2.2"), (["example.com"], ""), (None, "localhost")]
    for sans, host in good:
        assert ok(sans, host), (sans, host)
    for sans, host in bad:
        assert not ok(sans, host), (sans, host)


@test
def tls_host_faults():
    """a hostile network: bit flips in handshake and data records, cut connections, no close_notify, garbage, plain HTTP, fatal alerts, a downgrade marker"""
    if skip_without_crypto("tls_host_faults"):
        return
    T.reset()
    ca, leaf = pki()
    ctx = T.Ctx(ca.pem)

    def via_proxy(srv, mutate, **kw):
        px = T.Proxy(srv.port, mutate)
        return px, T.Client(socket.create_connection(("127.0.0.1", px.port), timeout=30), **kw)

    for kw in ({}, {"max_version": ssl.TLSVersion.TLSv1_2}):
        with T.Server(T.server_context(leaf, **kw)) as srv:
            def flip_handshake(d, i, rec, kw=kw):
                if d == "s2c" and ((not kw and rec[0] == 23) or (kw and rec[0] == 22 and i >= 4)):
                    b = bytearray(rec)
                    b[-1] ^= 1
                    return [bytes(b)]
                return [rec]
            px, c = via_proxy(srv, flip_handshake, ctx=ctx)
            want(c.handshake(), "DECRYPT")                              # bad MAC in the encrypted part of the handshake
            c.close(); px.stop()
        with T.Server(T.server_context(leaf, **kw)) as srv:
            state = {"armed": False}

            def flip_data(d, i, rec):
                if d == "s2c" and rec[0] == 23 and state["armed"]:
                    b = bytearray(rec)
                    b[10] ^= 4
                    return [bytes(b)]
                return [rec]
            px, c = via_proxy(srv, flip_data, ctx=ctx)
            assert c.handshake() == 0
            state["armed"] = True
            c.write(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            data, rc = c.read_all()
            want(rc, "DECRYPT")                                          # bad MAC in application data
            c.close(); px.stop()
        for cut in (1, 2, 3):
            with T.Server(T.server_context(leaf, **kw)) as srv:
                def hang_up(d, i, rec, cut=cut):
                    if d == "s2c" and i >= cut:
                        raise EOFError
                    return [rec]
                px, c = via_proxy(srv, hang_up, ctx=ctx)
                want(c.handshake(), "IO")                                # truncated handshake
                c.close(); px.stop()

        def no_close_notify(s):
            s.recv(4096)
            s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")
            time.sleep(0.1)
            s.shutdown(socket.SHUT_RDWR)
        with T.Server(T.server_context(leaf, **kw), handler=no_close_notify) as srv:
            rc, data, c = T.fetch(srv, ctx=ctx)
            assert rc == 0 and b"hello" in data
            want(c.read(), "TRUNCATED")                                  # the end of the data is not authenticated
            c.close()
    with T.Server(T.server_context(leaf, min_version=ssl.TLSVersion.TLSv1_3)) as srv:
        rc, data, c = T.fetch(srv, ctx=ctx, flags=T.TLS_F_TLS12_ONLY)
        want(rc, "ALERT")
        assert c.alert() in ("protocol_version", "handshake_failure"), c.alert()
        c.close()
    for payload in (b"HTTP/1.1 400 Bad Request\r\n\r\n", os.urandom(300), b"\x16\x03\x03\xff\xff" + b"A" * 100, b"\x16\x03\x01\x00\x04\x02\x00\x00\x00"):
        def junk(conn, payload=payload):
            conn.recv(1000)
            conn.sendall(payload)
            time.sleep(0.2)
            conn.close()
        with T.Server(None, raw_handler=junk) as srv:
            rc, data, c = T.fetch(srv, ctx=ctx)
            assert rc < 0, rc
            c.close()
    with T.Server(T.server_context(leaf, max_version=ssl.TLSVersion.TLSv1_2)) as srv:
        def downgrade(d, i, rec):
            if d == "s2c" and i == 0 and rec[0] == 22 and rec[5] == 2:
                b = bytearray(rec)
                b[11 + 24:11 + 32] = b"DOWNGRD\x01"                      # "I could have done TLS 1.3 but you did not ask"
                return [bytes(b)]
            return [rec]
        px, c = via_proxy(srv, downgrade, ctx=ctx)
        want(c.handshake(), "VERSION")
        c.close(); px.stop()
    with T.Server(T.server_context(leaf)) as srv:
        def tamper_hello(d, i, rec):
            if d == "s2c" and i == 0 and rec[0] == 22 and rec[5] == 2:
                b = bytearray(rec)
                b[11 + 5] ^= 1                                           # a changed ServerHello can only end in failed keys / Finished
                return [bytes(b)]
            return [rec]
        px, c = via_proxy(srv, tamper_hello, ctx=ctx)
        assert c.handshake() in (T.E["DECRYPT"], T.E["FINISHED"], T.E["HANDSHAKE_SIG"])
        c.close(); px.stop()


@test
def tls_host_roots():
    """the shipped CA bundle: every root loads and, apart from the SHA-1 signed ones, verifies its own signature (RSA and ECDSA on real certificates)"""
    if skip_without_crypto("tls_host_roots"):
        return
    T.reset()
    bundle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "initrd", "Hakurei", "etc", "ssl", "certs", "ca-bundle.pem")
    assert os.path.exists(bundle), "run python tools/mkcabundle.py"
    text = open(bundle, encoding="utf-8").read()
    ctx = T.Ctx(text)
    pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", text, re.S)
    assert len(pems) >= 100 and ctx.count() == len(pems) and ctx.skipped() == 0, (len(pems), ctx.count(), ctx.skipped())
    from cryptography import x509 as cx
    L = T.lib()
    unsupported = 0
    for pem in pems:
        der = ssl.PEM_cert_to_DER_cert(pem)
        rc = L.tls_x509_self_check(der, len(der))
        cert = cx.load_der_x509_certificate(der)
        if rc != 0:
            assert rc == T.E["CERT_UNSUPPORTED"] and "sha1" in cert.signature_algorithm_oid._name.lower(), (cert.subject.rfc4514_string(), T.ERRORS.get(rc, rc))
            unsupported += 1
    assert unsupported <= 8, unsupported
    for name in (b"Microsoft", b"ISRG Root X1", b"DigiCert", b"GlobalSign", b"Amazon Root CA 1"):      # sanity: the usual suspects are in there
        assert name.lower() in text.lower().encode() or name in text.encode(), name


# ------------------------------------------------------------------------------------------------------------ in the guest

import case_net  # noqa: E402  (registers the network tests too; harmless, the module is imported only once)
from case_net import Guest  # noqa: E402


def put_file(g, path, text):
    """Types a small text file into the guest (there is no other way in without a disk)."""
    g.run(f"rm -f {path}")
    lines = text.strip().splitlines()
    for i in range(0, len(lines), 3):
        g.run("; ".join(f"echo '{ln}' >> {path}" for ln in lines[i:i + 3]), timeout=60)


def start_http_redirector(target_url, final_body=None):
    """A plain HTTP server on 127.0.0.1 that answers every request with a 302 to target_url (or serves final_body)."""
    import http.server

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if final_body is not None:
                self.send_response(200)
                self.send_header("Content-Length", str(len(final_body)))
                self.end_headers()
                self.wfile.write(final_body)
            else:
                self.send_response(302)
                self.send_header("Location", target_url)
                self.send_header("Content-Length", "0")
                self.end_headers()

        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


@test
def tls_guest_fetch():
    """fetch and wget over HTTPS inside the system: verified connections (TLS 1.3 and 1.2, all ciphers, AES-NI and portable AES), --cacert, -v, redirects http <-> https, SNI by name, a pinned certificate"""
    if skip_without_crypto("tls_guest_fetch"):
        return
    ca = T.issue("Guest Test CA", T.new_key("ec256"), ca=True, path_len=0)
    leaf = T.issue("tls.test", T.new_key("ec256"), issuer=ca, sans=["tls.test", "10.0.2.2"])
    rsa_ca = T.issue("Guest RSA CA", T.new_key("rsa2048"), ca=True, path_len=0)
    rsa_leaf = T.issue("tls12.test", T.new_key("rsa2048"), issuer=rsa_ca, sans=["tls12.test", "10.0.2.2"])
    body = (b"hello from the host over TLS\n" * 400)
    seen_names = []
    sctx = T.server_context(leaf)
    sctx.sni_callback = lambda sock, name, c: seen_names.append(name)
    sctx.set_alpn_protocols(["http/1.1"])
    sctx12 = T.server_context(rsa_leaf, max_version=ssl.TLSVersion.TLSv1_2, ciphers="ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-CHACHA20-POLY1305")
    final = start_http_redirector(None, final_body=b"the end of the redirects\n")
    final_port = final.server_address[1]
    with T.Server(sctx, handler=T.http_handler(body)) as srv, T.Server(sctx12, handler=T.http_handler(body)) as srv12:
        # https -> http: the TLS server answers with a redirect to the plain server (hand-written handler)
        def to_plain(s):
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    return
                data += chunk
            s.sendall(f"HTTP/1.1 302 Found\r\nLocation: http://10.0.2.2:{final_port}/done\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".encode())
            s.unwrap()
        with T.Server(T.server_context(leaf), handler=to_plain) as srv_redir:
            plain_redirector = start_http_redirector(f"https://10.0.2.2:{srv.port}/from-http")
            with Guest("t-tls-fetch", net="virtio-net-pci", root=True) as g:
                put_file(g, "/tmp/ca.pem", ca.pem)
                put_file(g, "/tmp/rsa-ca.pem", rsa_ca.pem)
                g.run("echo '10.0.2.2 tls.test tls12.test' >> /etc/hosts")
                out, rc = g.run(f"fetch -v --cacert /tmp/ca.pem https://tls.test:{srv.port}/ | wc -c", timeout=240)
                assert str(len(body)) in out, out[-800:]
                assert "certificate verified" in out and "TLS 1.3" in out and "subject: O=TouhouOS Test, CN=tls.test" in out, out[-1200:]
                assert "issuer:  O=TouhouOS Test, CN=Guest Test CA" in out and "ALPN: http/1.1" in out, out
                assert seen_names and seen_names[-1] == "tls.test", seen_names                      # SNI carried the name
                for cipher, suite in (("aes128", "TLS_AES_128_GCM_SHA256"), ("aes256", "TLS_AES_256_GCM_SHA384"), ("chacha20", "TLS_CHACHA20_POLY1305_SHA256")):
                    for soft in ("", "--soft-aes"):
                        out, rc = g.run(f"fetch -v --cacert /tmp/ca.pem --cipher {cipher} {soft} -O /tmp/body.txt https://10.0.2.2:{srv.port}/ 2>&1; echo rc=$?; wc -c < /tmp/body.txt", timeout=240)
                        assert f"TLS 1.3, {suite}, certificate verified" in out and "rc=0" in out and str(len(body)) in out, (cipher, soft, out[-700:])
                out, rc = g.run(f"fetch -v --cacert /tmp/rsa-ca.pem https://tls12.test:{srv12.port}/ | wc -c", timeout=240)
                assert "TLS 1.2" in out and "certificate verified" in out and str(len(body)) in out, out[-900:]
                out, rc = g.run(f"fetch -v --tls12 --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/ | wc -c", timeout=240)
                assert "TLS 1.2" in out and str(len(body)) in out, out[-900:]
                out, rc = g.run(f"cd /tmp && wget -q --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/some/file.txt; echo rc=$?; ls -l file.txt | cut -c1-60; cmp file.txt /tmp/body.txt && echo same", timeout=240)
                assert "rc=0" in out and "same" in out, out[-500:]
                # redirects across the schemes: http -> https -> http
                out, rc = g.run(f"fetch --cacert /tmp/ca.pem http://10.0.2.2:{plain_redirector.server_address[1]}/start | wc -c", timeout=240)
                assert str(len(body)) in out, out[-600:]
                out, rc = g.run(f"fetch --cacert /tmp/ca.pem https://10.0.2.2:{srv_redir.port}/start 2>&1", timeout=240)
                assert "the end of the redirects" in out and "redirected from https to http" in out, out[-700:]
                # a pinned certificate: trusting the server's own certificate
                put_file(g, "/tmp/leaf.pem", leaf.pem)
                out, rc = g.run(f"fetch -q --cacert /tmp/leaf.pem https://10.0.2.2:{srv.port}/ | wc -c", timeout=240)
                assert str(len(body)) in out, out[-400:]
                # the machine's own list of authorities does not know our test CA
                out, rc = g.run(f"fetch https://10.0.2.2:{srv.port}/; echo rc=$?", timeout=240)
                assert "unknown CA" in out and "rc=5" in out, out[-700:]
                # SSL_CERT_FILE names another bundle
                out, rc = g.run(f"SSL_CERT_FILE=/tmp/ca.pem fetch -q https://10.0.2.2:{srv.port}/ | wc -c", timeout=240)
                assert str(len(body)) in out, out[-400:]
    plain_redirector.shutdown()
    final.shutdown()


@test
def tls_guest_refusals():
    """fetch refuses bad certificates (wrong name, expired, not yet valid, unknown CA, self-signed) with exit status 5 and says why; -k continues with a loud warning"""
    if skip_without_crypto("tls_guest_refusals"):
        return
    now = T.now_utc()
    day = datetime.timedelta(days=1)
    ca = T.issue("Refusal CA", T.new_key("ec256"), ca=True, path_len=0)
    other = T.issue("Stranger CA", T.new_key("ec256"), ca=True, path_len=0)
    cases = {
        "good": (T.issue("g", T.new_key("ec256"), issuer=ca, sans=["10.0.2.2"]), None),
        "wrong name": (T.issue("w", T.new_key("ec256"), issuer=ca, sans=["somewhere.else"]), "not valid for this host name"),
        "expired": (T.issue("e", T.new_key("ec256"), issuer=ca, sans=["10.0.2.2"], not_before=now - 10 * day, not_after=now - day), "has expired"),
        "future": (T.issue("f", T.new_key("ec256"), issuer=ca, sans=["10.0.2.2"], not_before=now + day, not_after=now + 9 * day), "not valid yet"),
        "unknown CA": (T.issue("u", T.new_key("ec256"), issuer=other, sans=["10.0.2.2"]), "unknown CA"),
        "self-signed": (T.issue("s", T.new_key("ec256"), sans=["10.0.2.2"]), "self-signed"),
    }
    servers = {name: T.Server(T.server_context(cert)) for name, (cert, _) in cases.items()}
    try:
        with Guest("t-tls-refuse", net="virtio-net-pci") as g:
            put_file(g, "/tmp/ca.pem", ca.pem)
            for name, (cert, message) in cases.items():
                port = servers[name].port
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{port}/ 2>&1 | head -12; echo status=$?", timeout=240)
                status = g.out(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{port}/ > /dev/null 2>&1; echo status=$?", timeout=240)
                if message is None:
                    assert "status=0" in status, (name, out[-500:])
                    continue
                assert message in out and "status=5" in status, (name, out[-700:], status)
                if name in ("expired", "future"):
                    assert "clock says" in out, out                                   # the hint about the system clock
                out, rc = g.run(f"fetch -q -k --cacert /tmp/ca.pem https://10.0.2.2:{port}/ 2>&1 | head -5; echo status=$?", timeout=240)
                assert "WARNING" in out and "switched off" in out and message in out, (name, out[-700:])
                assert g.out(f"fetch -q -k --cacert /tmp/ca.pem https://10.0.2.2:{port}/ > /dev/null 2>&1; echo status=$?", timeout=240).strip().endswith("status=0")
    finally:
        for s in servers.values():
            s.stop()


@test
def tls_guest_faults():
    """the guest meets a corrupted record, a connection cut during the handshake and a server that never answers: errors, not hangs"""
    if skip_without_crypto("tls_guest_faults"):
        return
    ca = T.issue("Fault CA", T.new_key("ec256"), ca=True, path_len=0)
    leaf = T.issue("f", T.new_key("ec256"), issuer=ca, sans=["10.0.2.2"])
    big = b"x" * 40000
    with T.Server(T.server_context(leaf), handler=T.http_handler(big)) as srv:
        state = {"flip_after": 3}

        def flip(d, i, rec):                                         # corrupt the 3rd record from the server onwards
            if d == "s2c" and i == state["flip_after"]:
                b = bytearray(rec)
                b[-3] ^= 0x10
                return [bytes(b)]
            return [rec]

        def cut(d, i, rec):
            if d == "s2c" and i >= 2:
                raise EOFError
            return [rec]

        def silent(d, i, rec):                                       # swallow everything the server says
            return []
        def hang_up_framed(s):
            s.recv(4096)
            s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello")
            time.sleep(0.2)
            s.shutdown(socket.SHUT_RDWR)

        def hang_up_unframed(s):
            s.recv(4096)
            s.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nthis is all there is, or is it?")
            time.sleep(0.2)
            s.shutdown(socket.SHUT_RDWR)
        with T.Server(T.server_context(leaf), handler=hang_up_framed) as framed, T.Server(T.server_context(leaf), handler=hang_up_unframed) as unframed, \
                T.Proxy(srv.port, flip) as px_flip, T.Proxy(srv.port, cut) as px_cut, T.Proxy(srv.port, silent) as px_silent:
            with Guest("t-tls-faults", net="virtio-net-pci") as g:
                put_file(g, "/tmp/ca.pem", ca.pem)
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/ | wc -c", timeout=240)
                assert out.strip().splitlines()[-1].strip() == "40000", out[-400:]
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{px_flip.port}/ 2>&1 | wc -c; fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{px_flip.port}/ > /dev/null; echo status=$?", timeout=240)
                assert "status=0" not in out, out[-500:]
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{px_cut.port}/ 2>&1 | head -3; fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{px_cut.port}/ > /dev/null 2>&1; echo status=$?", timeout=240)
                assert "status=5" in out and ("closed" in out or "failed" in out), out[-500:]
                out, rc = g.run(f"fetch -q -T 3 --cacert /tmp/ca.pem https://10.0.2.2:{px_silent.port}/ 2>&1 | head -3; fetch -q -T 3 --cacert /tmp/ca.pem https://10.0.2.2:{px_silent.port}/ > /dev/null 2>&1; echo status=$?", timeout=240)
                assert "status=5" in out, out[-500:]
                # the server hangs up without close_notify: fine when the length was announced, an error when the end of the data is only implied
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{framed.port}/; echo status=$?", timeout=240)
                assert "hello" in out and "status=0" in out, out[-400:]
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{unframed.port}/ 2>&1; echo status=$?", timeout=240)
                assert "without close_notify" in out and "status=1" in out, out[-500:]


@test
def tls_guest_cpu_without_aesni_or_rdrand():
    """on a CPU with neither AES-NI nor RDRAND/RDSEED the kernel says its randomness is poor and fetch refuses until --weak-random; the portable AES and ChaCha20 then work"""
    if skip_without_crypto("tls_guest_cpu_without_aesni_or_rdrand"):
        return
    ca = T.issue("Weak CA", T.new_key("ec256"), ca=True, path_len=0)
    leaf = T.issue("w", T.new_key("ec256"), issuer=ca, sans=["10.0.2.2"])
    body = b"portable crypto works\n" * 100
    old = os.environ.get("TOUHOU_CPU")
    os.environ["TOUHOU_CPU"] = "qemu64"                              # no AES, PCLMULQDQ, RDRAND, RDSEED
    try:
        with T.Server(T.server_context(leaf), handler=T.http_handler(body)) as srv:
            with Guest("t-tls-qemu64", net="virtio-net-pci") as g:
                rnd = g.out("cat /proc/random")
                assert "ready: no" in rnd and "jitter only" in rnd, rnd
                put_file(g, "/tmp/ca.pem", ca.pem)
                out, rc = g.run(f"fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/ 2>&1 | head -4; fetch -q --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/ > /dev/null 2>&1; echo status=$?", timeout=240)
                assert "random number generator is not seeded" in out and "status=5" in out, out[-600:]
                out, rc = g.run(f"fetch -qv --weak-random --cacert /tmp/ca.pem -O /tmp/b.txt https://10.0.2.2:{srv.port}/; echo status=$?; wc -c < /tmp/b.txt", timeout=240)
                assert "status=0" in out and str(len(body)) in out, out[-500:]
                out, rc = g.run(f"fetch -q --weak-random --cipher chacha20 --cacert /tmp/ca.pem https://10.0.2.2:{srv.port}/ | wc -c", timeout=240)
                assert str(len(body)) in out, out[-300:]
                # events (the keyboard we are typing on) feed the generator: the estimate has moved
                after = g.out("cat /proc/random")
                m1 = re.search(r"estimated entropy: (\d+) bits", rnd)
                m2 = re.search(r"estimated entropy: (\d+) bits", after)
                assert int(m2.group(1)) >= int(m1.group(1)), (rnd, after)
    finally:
        if old is None:
            os.environ.pop("TOUHOU_CPU", None)
        else:
            os.environ["TOUHOU_CPU"] = old


@test
def tls_guest_random():
    """/dev/urandom on the normal CPU: seeded from RDSEED/RDRAND (ready), two draws differ and 32 KB of it does not compress"""
    with Guest("t-tls-random") as g:
        info = g.out("cat /proc/random")
        assert "ChaCha20" in info and "ready: yes" in info, info
        a = g.out("dd if=/dev/urandom bs=4096 count=8 2>/dev/null | md5sum")
        b = g.out("dd if=/dev/urandom bs=4096 count=8 2>/dev/null | md5sum")
        assert re.search(r"[0-9a-f]{32}", a) and re.search(r"[0-9a-f]{32}", b) and a != b, (a, b)
        # incompressible: gzip cannot make 32 KB of good random numbers smaller
        out = g.out("dd if=/dev/urandom bs=4096 count=8 2>/dev/null | gzip -c | wc -c")
        n = int(re.findall(r"\d+", out)[-1])
        assert n > 32768, out


# ------------------------------------------------------------------------------------------------------------ the real internet

def internet_available():
    try:
        socket.create_connection(("example.com", 443), timeout=4).close()
        return True
    except OSError:
        return False


@test
def tls_guest_internet():
    """optional: the guest fetches real public HTTPS pages (example.com, a Let's Encrypt site) against the shipped Mozilla roots, and rejects badssl.com's expired certificate; skipped when the host is offline"""
    if not internet_available():
        print("SKIP tls_guest_internet: this machine cannot reach example.com:443")
        return
    with Guest("t-tls-internet", net="virtio-net-pci") as g:
        out, rc = g.run("fetch -q -O /tmp/e.html https://example.com/; echo status=$?; grep -c -i 'example domain' /tmp/e.html", timeout=300)
        assert "status=0" in out and out.strip().splitlines()[-1].strip() != "0", out[-500:]
        out, rc = g.run("fetch -q https://www.wikipedia.org/ | wc -c", timeout=300)
        assert int(re.findall(r"\d+", out)[-1]) > 20000, out
        out, rc = g.run("fetch -q https://expired.badssl.com/ 2>&1 | head -2; fetch -q https://expired.badssl.com/ > /dev/null 2>&1; echo status=$?", timeout=300)
        assert "has expired" in out and "status=5" in out, out[-500:]
