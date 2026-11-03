"""TLS and HTTPS in the guest: libtls, libhttp and https_get against servers on the host (docs/TLS.md).

The guest reaches the host as 10.0.2.2 (QEMU's user-mode networking); its /etc/hosts maps tls.test and wrong.test there, and it
fetches the test CA over plain HTTP with fetch (which case_net.py covers). The servers:
  * openssl s_server in WSL (Windows sees WSL's listening ports on 127.0.0.1, which QEMU maps to 10.0.2.2): the handshake
    matrix (both versions, every suite, ECDSA/RSA/Ed25519 certificates, HelloRetryRequest) and the refused certificates;
  * Python on the host: an HTTPS server for the HTTP-level checks (a 20 MB file, chunked, gzip, redirects, HEAD, 404) and the
    misbehaving ones (a server that stalls, one that cuts the handshake, plain HTTP on the TLS port).
The test CA and the good certificates are made by the openssl command in WSL; the deliberately broken ones (expired, not yet
valid, a bad signature, an unknown CA) by Python's cryptography, signed with that CA's key.

Every https_get runs with --ca /tmp/ca.pem (no trust store is shipped). The guest clock may run slower than wall time: nothing
here sleeps and then expects an effect. Run through the limiter: python tools/qemu-slot.py -- python tests/run_all.py tls_
"""
import datetime
import gzip
import hashlib
import http.server
import os
import random
import re
import shutil
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from harness import test
from case_net import Guest

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    HAVE_CRYPTO = True
except ImportError:                                                     # pragma: no cover
    HAVE_CRYPTO = False

WSL = shutil.which("wsl.exe") or shutil.which("wsl")


def wsl_path(p):
    p = str(Path(p).resolve())
    return f"/mnt/{p[0].lower()}{p[2:].replace(chr(92), '/')}" if os.name == "nt" else p


def openssl(*args, cwd=None):
    """the openssl command: in WSL on Windows (as the brief asks), the local one elsewhere"""
    cmd = ([WSL, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "openssl"] if os.name == "nt" else ["openssl"]) + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)


def have_tools():
    if os.name == "nt" and not WSL:
        return "WSL is not installed"
    r = openssl("version")
    if r.returncode != 0:
        return "no openssl command" + (" in WSL" if os.name == "nt" else "")
    if not HAVE_CRYPTO:
        return "Python's cryptography package is missing on the host"
    return None


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ------------------------------------------------------------------------------------------------------------ certificates

class Pki:
    """The test CA and server certificates in a temporary folder that both Windows and WSL can read."""

    def __init__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="tls-pki-"))
        d = wsl_path(self.dir)
        names = "subjectAltName=DNS:tls.test,IP:10.0.2.2"
        (self.dir / "leaf.ext").write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                                           "extendedKeyUsage=serverAuth\nauthorityKeyIdentifier=keyid\nsubjectKeyIdentifier=hash\n" + names + "\n")
        self.must(openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", f"{d}/ca.key",
                          "-out", f"{d}/ca.pem", "-days", "60", "-subj", "/CN=TouhouOS Test Root/O=TouhouOS tests",
                          "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign"))
        for name, alg in (("ec", ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"]), ("rsa", ["-newkey", "rsa:2048"]),
                          ("ed", ["-newkey", "ed25519"])):
            self.must(openssl("req", *alg, "-nodes", "-keyout", f"{d}/{name}.key", "-out", f"{d}/{name}.csr", "-subj", "/CN=tls.test/O=TouhouOS tests"))
            self.must(openssl("x509", "-req", "-in", f"{d}/{name}.csr", "-CA", f"{d}/ca.pem", "-CAkey", f"{d}/ca.key", "-CAcreateserial",
                              "-days", "30", "-out", f"{d}/{name}.pem", "-extfile", f"{d}/leaf.ext"))
        self.ca_cert = x509.load_pem_x509_certificate((self.dir / "ca.pem").read_bytes())
        self.ca_key = serialization.load_pem_private_key((self.dir / "ca.key").read_bytes(), None)
        now = datetime.datetime.now(datetime.timezone.utc)
        day = datetime.timedelta(days=1)
        self.broken("expired", now - 60 * day, now - day)
        self.broken("young", now + 10 * day, now + 60 * day)
        self.broken("badsig", now - day, now + 30 * day, corrupt=True)
        other_key = ec.generate_private_key(ec.SECP256R1())
        other = self.make(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Somebody Else's Root")]), other_key, other_key, None, now - day, now + 30 * day, ca=True)
        self.broken("unknownca", now - day, now + 30 * day, issuer=(other, other_key))
        self.broken("selfsigned", now - day, now + 30 * day, self_signed=True)

    @staticmethod
    def must(r):
        if r.returncode != 0:
            raise RuntimeError(f"openssl failed: {r.stdout}{r.stderr}")

    def make(self, subject, key, signer_key, issuer_cert, nb, na, ca=False, sans=True):
        b = x509.CertificateBuilder().subject_name(subject).public_key(key.public_key()).serial_number(x509.random_serial_number())
        b = b.issuer_name(issuer_cert.subject if issuer_cert else subject).not_valid_before(nb).not_valid_after(na)
        b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
        if not ca and sans:
            import ipaddress
            b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName("tls.test"), x509.IPAddress(ipaddress.ip_address("10.0.2.2"))]), critical=False)
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        return b.sign(signer_key, hashes.SHA256())

    def broken(self, name, nb, na, corrupt=False, issuer=None, self_signed=False):
        key = ec.generate_private_key(ec.SECP256R1())
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "tls.test")])
        if self_signed:
            cert = self.make(subject, key, key, None, nb, na)
        else:
            icert, ikey = issuer or (self.ca_cert, self.ca_key)
            cert = self.make(subject, key, ikey, icert, nb, na)
        der = bytearray(cert.public_bytes(serialization.Encoding.DER))
        if corrupt:
            der[-3] ^= 0x01                                           # inside the signature's integers: still DER, wrong value
        pem = b"-----BEGIN CERTIFICATE-----\n" + __import__("base64").encodebytes(bytes(der)) + b"-----END CERTIFICATE-----\n"
        (self.dir / f"{name}.pem").write_bytes(pem)
        (self.dir / f"{name}.key").write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

    def files(self, name):
        return str(self.dir / f"{name}.pem"), str(self.dir / f"{name}.key")

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


# ------------------------------------------------------------------------------------------------------------ servers

class SServer:
    """openssl s_server -www (in WSL on Windows): answers every GET with a status page, one connection after the other."""

    def __init__(self, pki, cert, *args):
        self.port = free_port()
        d = wsl_path(pki.dir)
        cmd = ["s_server", "-accept", f"0.0.0.0:{self.port}" if os.name == "nt" else f"127.0.0.1:{self.port}", "-www",
               "-cert", f"{d}/{cert}.pem", "-key", f"{d}/{cert}.key", *args]
        full = ([WSL, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "openssl"] if os.name == "nt" else ["openssl"]) + cmd
        self.p = subprocess.Popen(full, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 20
        while time.time() < deadline:                                # ready when Windows can connect to the port
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return
            except OSError:
                if self.p.poll() is not None:
                    raise RuntimeError("s_server exited: " + self.p.stdout.read())
                time.sleep(0.2)
        raise RuntimeError("s_server did not start listening")

    def stop(self):
        self.p.kill()
        self.p.wait()


class Files(threading.Thread):
    """Plain HTTP for the guest's fetch: the files of one folder."""

    def __init__(self, folder):
        super().__init__(daemon=True)
        handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=str(folder), **k)    # noqa: E731
        http.server.SimpleHTTPRequestHandler.log_message = lambda *a: None
        self.httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
        self.port = self.httpd.server_address[1]
        self.start()

    def run(self):
        self.httpd.serve_forever()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class Raw(threading.Thread):
    """A TCP server on the host whose every connection is handed to fn(sock); with ctx, wrapped in TLS first."""

    def __init__(self, fn, ctx=None):
        super().__init__(daemon=True)
        self.fn, self.ctx = fn, ctx
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.done = False
        self.start()

    def run(self):
        while not self.done:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            threading.Thread(target=self.one, args=(c,), daemon=True).start()

    def one(self, c):
        try:
            c.settimeout(120)
            if self.ctx:
                c = self.ctx.wrap_socket(c, server_side=True)
            self.fn(c)
        except Exception:  # noqa: BLE001
            pass
        finally:
            try:
                c.close()
            except Exception:  # noqa: BLE001
                pass

    def stop(self):
        self.done = True
        self.join(5)
        self.sock.close()


def read_request(c):
    req = b""
    while b"\r\n\r\n" not in req:
        d = c.recv(4096)
        if not d:
            break
        req += d
    return req


_BIG = []
PAGE = b"<html><body>Hakurei Shrine over TLS</body></html>\n" * 40


def big():
    """the 20 MiB download, made when a test first needs it (not at import: run_all.py imports every case file)"""
    if not _BIG:
        _BIG.append(random.Random(2026).randbytes(20 * 1024 * 1024))
    return _BIG[0]


def https_site(c):
    """the HTTPS server of tls_guest_downloads"""
    req = read_request(c)
    path = req.split(b" ")[1] if b" " in req else b"/"
    head = req.startswith(b"HEAD ")

    def send(status, headers, body=b""):
        c.sendall(b"HTTP/1.1 " + status + b"\r\n" + b"".join(h + b"\r\n" for h in headers) + b"\r\n" + (b"" if head else body))

    if path == b"/big":
        data = big()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % len(data))
        if not head:
            view = memoryview(data)
            for i in range(0, len(data), 1 << 16):
                c.sendall(view[i:i + (1 << 16)])
    elif path == b"/page":
        send(b"200 OK", [b"Content-Type: text/html", b"Content-Length: %d" % len(PAGE)], PAGE)
    elif path == b"/chunked":
        body = b"".join(b"%x\r\n" % len(PAGE[i:i + 777]) + PAGE[i:i + 777] + b"\r\n" for i in range(0, len(PAGE), 777)) + b"0\r\n\r\n"
        send(b"200 OK", [b"Transfer-Encoding: chunked"], body)
    elif path == b"/gzip":
        z = gzip.compress(PAGE)
        send(b"200 OK", [b"Content-Encoding: gzip", b"Content-Length: %d" % len(z)], z)
    elif path == b"/redirect":
        send(b"302 Found", [b"Location: /page", b"Content-Length: 0"])
    elif path.startswith(b"/to-plain"):
        send(b"301 Moved", [b"Location: " + path[len(b"/to-plain?"):], b"Content-Length: 0"])
    else:
        send(b"404 Not Found", [b"Content-Length: 10"], b"not here!\n")
    if isinstance(c, ssl.SSLSocket):
        try:
            c.unwrap()
        except Exception:  # noqa: BLE001
            pass


class Setup:
    """The PKI, the plain file server and a booted guest with tls.test in /etc/hosts and the CA in /tmp/ca.pem."""

    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.pki = Pki()
        self.files = Files(self.pki.dir)
        self.g = Guest(self.name, net="virtio-net-pci", root=True).__enter__()
        self.run("echo '10.0.2.2 tls.test wrong.test' >> /etc/hosts")
        out, rc = self.run(f"fetch -q -O /tmp/ca.pem http://10.0.2.2:{self.files.port}/ca.pem && grep -c BEGIN /tmp/ca.pem", timeout=120)
        assert rc == 0 and "1" in out, out
        return self

    def run(self, cmd, timeout=240):
        return self.g.run(cmd, timeout=timeout)

    def get(self, args, timeout=240):
        """https_get with the test CA; returns (output, exit status)"""
        return self.run(f"https_get --ca /tmp/ca.pem {args}", timeout=timeout)

    def __exit__(self, *exc):
        try:
            self.g.__exit__(*exc)
        finally:
            self.files.stop()
            self.pki.cleanup()


def skip_reason():
    why = have_tools()
    if why:
        print(f"SKIP: {why}")
    return why


# ------------------------------------------------------------------------------------------------------------ tests

@test
def tls_guest_handshakes():
    """https_get against openssl s_server: TLS 1.3 with each of its three suites, TLS 1.2 with the six ECDHE suites (ECDSA and RSA certificates), an Ed25519 certificate, HelloRetryRequest to P-256; the -v report"""
    if skip_reason():
        return
    with Setup("t-tls-handshakes") as s:
        servers = {name: SServer(s.pki, cert, *args) for name, cert, args in (
            ("13ec", "ec", ["-tls1_3"]), ("13rsa", "rsa", ["-tls1_3"]), ("13ed", "ed", ["-tls1_3"]), ("13hrr", "ec", ["-tls1_3", "-groups", "P-256"]),
            ("12ec", "ec", ["-tls1_2"]), ("12rsa", "rsa", ["-tls1_2"]))}
        try:
            for cipher, suite in (("aes128", "TLS_AES_128_GCM_SHA256"), ("aes256", "TLS_AES_256_GCM_SHA384"), ("chacha20", "TLS_CHACHA20_POLY1305_SHA256")):
                out, rc = s.get(f"-v --cipher {cipher} https://tls.test:{servers['13ec'].port}/")
                assert rc == 0, out[-2500:]
                assert f"TLS 1.3, {suite}, key exchange x25519, server signature ecdsa_secp256r1_sha256" in out, out[-2500:]
                assert "New, TLSv1.3, Cipher is " + suite in out, out[-1500:]           # the server's page says the same
            out, rc = s.get(f"-v https://tls.test:{servers['13ec'].port}/")
            for line in ("the verified path, leaf to root", "0 CN=tls.test, O=TouhouOS tests", "names   tls.test, 10.0.2.2",
                         "1 CN=TouhouOS Test Root, O=TouhouOS tests   (root from the trust store)", "key     EC P-256, signed with ECDSA SHA-256",
                         "clock: system clock"):
                assert line in out, (line, out[-3000:])
            out, rc = s.get(f"-v https://tls.test:{servers['13rsa'].port}/")
            assert rc == 0 and "server signature rsa_pss_rsae_sha256" in out, out[-2500:]
            out, rc = s.get(f"-v https://tls.test:{servers['13ed'].port}/")
            assert rc == 0 and "server signature ed25519" in out, out[-2500:]
            out, rc = s.get(f"-v https://tls.test:{servers['13hrr'].port}/")
            assert rc == 0 and "key exchange secp256r1" in out and "after a HelloRetryRequest" in out, out[-2500:]
            for srv, auth in (("12ec", "ECDSA"), ("12rsa", "RSA")):
                for cipher, bulk in (("aes128", "AES_128_GCM_SHA256"), ("aes256", "AES_256_GCM_SHA384"), ("chacha20", "CHACHA20_POLY1305_SHA256")):
                    out, rc = s.get(f"-v --cipher {cipher} https://tls.test:{servers[srv].port}/")
                    assert rc == 0, out[-2500:]
                    assert f"TLS 1.2, TLS_ECDHE_{auth}_WITH_{bulk}" in out, out[-2500:]
            out, rc = s.get(f"-v https://10.0.2.2:{servers['12ec'].port}/")       # an address: the IP SAN, no SNI
            assert rc == 0, out[-2500:]
        finally:
            for v in servers.values():
                v.stop()


@test
def tls_guest_refusals():
    """fails closed (exit 3, a reason): wrong name, expired, not yet valid, unknown CA, self-signed, bad signature, no trust store, plain HTTP, a cut handshake, a stalling server; --insecure goes on, warning"""
    if skip_reason():
        return
    with Setup("t-tls-refusals") as s:
        servers = {c: SServer(s.pki, c, "-tls1_3") for c in ("ec", "expired", "young", "badsig", "unknownca", "selfsigned")}

        def cut(c):
            c.recv(4096)
            c.sendall(bytes([22, 3, 3, 0, 122, 2, 0, 0, 118, 3, 3]) + os.urandom(20))   # a ServerHello that stops in the middle

        def stall(c):
            c.recv(4096)
            time.sleep(60)

        def plain(c):
            c.recv(4096)
            c.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        raws = {"cut": Raw(cut), "stall": Raw(stall), "plain": Raw(plain)}
        try:
            cases = [
                (f"https://wrong.test:{servers['ec'].port}/", "is not valid for wrong.test: it names tls.test, 10.0.2.2"),
                (f"https://tls.test:{servers['expired'].port}/", "expired on"),
                (f"https://tls.test:{servers['young'].port}/", "is valid only from"),
                (f"https://tls.test:{servers['unknownca'].port}/", "does not lead to a trusted root"),
                (f"https://tls.test:{servers['selfsigned'].port}/", "is self-signed and not in the trust store"),
                (f"https://tls.test:{servers['badsig'].port}/", "signature in the certificate chain is invalid"),
                (f"https://tls.test:{raws['plain'].port}/", "did not answer in TLS"),
                (f"https://tls.test:{raws['cut'].port}/", "the connection ended in the middle of a record"),
                (f"--timeout 3 https://tls.test:{raws['stall'].port}/", "sent nothing for 3 s"),
            ]
            for args, reason in cases:
                out, rc = s.get(args)
                assert rc == 3, (args, rc, out[-1500:])
                assert reason in out, (args, reason, out[-1500:])
            out, rc = s.run(f"https_get https://tls.test:{servers['ec'].port}/")                # no --ca, no /etc/ssl/cacert.pem
            assert rc == 3 and "/etc/ssl/cacert.pem" in out and "--insecure" in out, out[-1500:]
            out, rc = s.run(f"SSL_CERT_FILE=/tmp/ca.pem https_get -q -o /tmp/x https://tls.test:{servers['ec'].port}/")
            assert rc == 0, out[-1500:]
            out, rc = s.run(f"https_get --insecure -o /tmp/x https://wrong.test:{servers['selfsigned'].port}/")
            assert rc == 0 and "WARNING: --insecure" in out and "Verification would have said" in out, out[-1500:]
            out, rc = s.get(f"--time {int(time.time()) + 90 * 86400} https://tls.test:{servers['ec'].port}/")   # a pinned clock past the end
            assert rc == 3 and "set by the program" in out, out[-1500:]
            out, rc = s.get(f"--require-clock https://tls.test:{servers['ec'].port}/")
            assert rc == 3 and "clock is not trusted" in out, out[-1500:]
            out, rc = s.get(f"-o /tmp/fail https://wrong.test:{servers['ec'].port}/")       # a refused download leaves no file
            assert rc == 3, out[-1500:]
            out, rc = s.run("ls /tmp/fail /tmp/fail.part 2>&1; echo listed")
            assert out.lower().count("no such") == 2, out
        finally:
            for v in servers.values():
                v.stop()
            for v in raws.values():
                v.stop()


@test
def tls_guest_downloads():
    """HTTP/1.1 over TLS from a Python server on the host: 20 MB with the right SHA-256 (and the speed), chunked and gzip pages, a redirect, https-to-http with the warning, HEAD, 404 (exit 4)"""
    if skip_reason():
        return
    with Setup("t-tls-downloads") as s:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*s.pki.files("ec"))
        site = Raw(https_site, ctx)
        plain = Raw(https_site)
        base = f"https://tls.test:{site.port}"
        try:
            t0 = time.time()
            out, rc = s.get(f"-o /tmp/big {base}/big; sha256sum /tmp/big", timeout=900)
            secs = time.time() - t0
            assert rc == 0 and hashlib.sha256(big()).hexdigest() in out, out[-1500:]
            print(f"     20 MiB over TLS into the guest in {secs:.1f} s of wall time ({len(big()) / secs / 1e6:.2f} MB/s)")
            want = hashlib.sha256(PAGE).hexdigest()
            for path in ("/page", "/chunked", "/gzip", "/redirect"):
                out, rc = s.get(f"-o /tmp/p {base}{path}; sha256sum /tmp/p")
                assert rc == 0 and want in out, (path, out[-1500:])
            out, rc = s.get(f"-v -o /tmp/p {base}/gzip")
            assert "Content-Encoding: gzip" in out and "(decompressed)" in out, out[-1500:]
            out, rc = s.get(f"-o /tmp/p '{base}/to-plain?http://tls.test:{plain.port}/page'; sha256sum /tmp/p")
            assert rc == 0 and "WARNING: redirected from https to plain http" in out and want in out, out[-1500:]
            out, rc = s.get(f"-I {base}/big")
            assert rc == 0 and f"Content-Length: {len(big())}" in out, out[-1500:]
            out, rc = s.get(f"-o /tmp/nf {base}/nothing; cat /tmp/nf")
            assert "the server answered 404" in out and "not here!" in out, out[-1500:]
            out, rc = s.get(f"-q {base}/nothing > /dev/null")
            assert rc == 4, (rc, out)
        finally:
            site.stop()
            plain.stop()


def internet():
    try:
        socket.create_connection(("example.com", 443), timeout=4).close()
        return True
    except OSError:
        return False


@test
def tls_guest_internet():
    """optional: example.com and www.wikipedia.org against Mozilla's roots (certifi on the host), expired.badssl.com refused; skipped without internet"""
    try:
        import certifi
    except ImportError:
        print("SKIP: the certifi package is not installed on the host")
        return
    if not internet():
        print("SKIP: the host cannot reach example.com")
        return
    folder = Path(tempfile.mkdtemp(prefix="tls-roots-"))
    shutil.copy(certifi.where(), folder / "roots.pem")
    files = Files(folder)
    try:
        with Guest("t-tls-internet", net="virtio-net-pci", root=True) as g:
            out, rc = g.run(f"fetch -q -O /etc/ssl/cacert.pem http://10.0.2.2:{files.port}/roots.pem || "
                            f"(mkdir -p /etc/ssl && fetch -q -O /etc/ssl/cacert.pem http://10.0.2.2:{files.port}/roots.pem)", timeout=180)
            for url in ("https://example.com/", "https://www.wikipedia.org/"):
                out, rc = g.run(f"https_get -v -o /tmp/page {url}", timeout=300)
                assert rc in (0, 4), (url, out[-2500:])
                assert "the verified path" in out, out[-2500:]
            out, rc = g.run("https_get https://expired.badssl.com/", timeout=300)
            assert rc == 3 and "expired" in out, out[-1500:]
    finally:
        files.stop()
        shutil.rmtree(folder, ignore_errors=True)
