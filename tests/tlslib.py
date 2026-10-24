"""Helpers for the TLS tests: libtls compiled for the build machine (tools/tlshost.py) driven through ctypes, a small certificate
factory built on the `cryptography` package, and TLS servers (Python's ssl module, which is OpenSSL) to talk to.

The same C sources run here and in the guest, so a handshake that works against OpenSSL on the host exercises exactly the code
that the guest's fetch and wget use; the guest tests (case_tls.py) then only have to prove that the operating system around it
(sockets, clock, random numbers, files) works.
"""
import ctypes
import datetime
import os
import socket
import ssl
import sys
import tempfile
import threading
import time
from ctypes import c_char_p, c_int, c_int64, c_long, c_size_t, c_uint, c_void_p
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from cryptography import x509  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec, rsa  # noqa: E402
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID  # noqa: E402

import tlshost  # noqa: E402

TLS_F_NO_VERIFY, TLS_F_TLS12_ONLY, TLS_F_TLS13_ONLY, TLS_F_WEAK_RANDOM, TLS_F_NO_AESNI = 1, 2, 4, 8, 0x10
TLS_S_AES128, TLS_S_AES256, TLS_S_CHACHA = 1, 2, 4

ERRORS = {
    0: "OK", -1: "ARG", -2: "NOMEM", -3: "IO", -4: "TRUNCATED", -5: "PROTOCOL", -6: "ALERT", -7: "VERSION", -8: "SUITE", -9: "DECRYPT",
    -10: "HANDSHAKE_SIG", -11: "FINISHED", -12: "ENTROPY", -20: "CERT_PARSE", -21: "CERT_UNTRUSTED", -22: "CERT_SELF_SIGNED",
    -23: "CERT_EXPIRED", -24: "CERT_NOT_YET_VALID", -25: "CERT_NAME", -26: "CERT_SIGNATURE", -27: "CERT_USAGE", -28: "CERT_UNSUPPORTED",
    -29: "CERT_EMPTY",
}
E = {name: code for code, name in ERRORS.items()}

SENDFN = ctypes.CFUNCTYPE(c_long, c_void_p, c_void_p, c_size_t)
RECVFN = ctypes.CFUNCTYPE(c_long, c_void_p, c_void_p, c_size_t)
RANDFN = ctypes.CFUNCTYPE(None, c_void_p, c_size_t)


class Io(ctypes.Structure):
    _fields_ = [("send", SENDFN), ("recv", RECVFN), ("user", c_void_p)]


class Config(ctypes.Structure):
    _fields_ = [("server_name", c_char_p), ("alpn", c_char_p), ("ctx", c_void_p), ("flags", c_uint), ("suites", c_uint),
                ("now", c_int64), ("random", RANDFN)]


class Info(ctypes.Structure):
    _fields_ = [("version", c_int), ("version_name", c_char_p), ("suite_name", c_char_p), ("group_name", c_char_p),
                ("alpn", ctypes.c_char * 16), ("verified", c_int), ("verify_error", c_int),
                ("subject", ctypes.c_char * 200), ("issuer", ctypes.c_char * 200), ("not_before", c_int64), ("not_after", c_int64),
                ("fingerprint", ctypes.c_ubyte * 32), ("chain_length", c_uint), ("hello_retry", c_int)]


_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = tlshost.load()
        L = _lib
        L.tls_ctx_new.restype = c_void_p
        L.tls_ctx_free.argtypes = [c_void_p]
        L.tls_ctx_load_pem.argtypes = [c_void_p, c_char_p, c_size_t]
        L.tls_ctx_load_der.argtypes = [c_void_p, c_char_p, c_size_t]
        L.tls_ctx_count.argtypes = [c_void_p]
        L.tls_ctx_count.restype = c_size_t
        L.tls_ctx_skipped.argtypes = [c_void_p]
        L.tls_ctx_skipped.restype = c_size_t
        L.tls_new.argtypes = [ctypes.POINTER(Io), ctypes.POINTER(Config)]
        L.tls_new.restype = c_void_p
        L.tls_handshake.argtypes = [c_void_p]
        L.tls_read.argtypes = [c_void_p, c_void_p, c_size_t]
        L.tls_read.restype = c_long
        L.tls_write.argtypes = [c_void_p, c_char_p, c_size_t]
        L.tls_write.restype = c_long
        L.tls_shutdown.argtypes = [c_void_p]
        L.tls_free.argtypes = [c_void_p]
        L.tls_get_info.argtypes = [c_void_p]
        L.tls_get_info.restype = ctypes.POINTER(Info)
        L.tls_alert_received.argtypes = [c_void_p]
        L.tls_last_error.argtypes = [c_void_p]
        L.tls_strerror.argtypes = [c_int]
        L.tls_strerror.restype = c_char_p
        L.tls_alert_name.argtypes = [c_int]
        L.tls_alert_name.restype = c_char_p
        L.tls_x509_match_name.argtypes = [c_char_p, c_size_t, c_char_p]
    return _lib


def reset():
    """Starts the DLL's bump allocator over. Only when nothing from earlier tests is still in use."""
    lib().tlshost_reset()


class Ctx:
    """A set of trusted roots."""

    def __init__(self, *pems):
        self.h = lib().tls_ctx_new()
        self.added = 0
        for pem in pems:
            self.add_pem(pem)

    def add_pem(self, pem):
        if isinstance(pem, str):
            pem = pem.encode()
        n = lib().tls_ctx_load_pem(self.h, pem, len(pem))
        assert n >= 0, n
        self.added += n
        return n

    def count(self):
        return lib().tls_ctx_count(self.h)

    def skipped(self):
        return lib().tls_ctx_skipped(self.h)


class Client:
    """libtls over a connected Python socket."""

    def __init__(self, sock, server_name="localhost", ctx=None, alpn=None, flags=0, suites=0, now=None, random_fn=None):
        self.sock = sock
        self.error = None

        def send(user, buf, n):
            try:
                self.sock.sendall(ctypes.string_at(buf, n))
                return n
            except OSError:
                return -1

        def recv(user, buf, n):
            try:
                data = self.sock.recv(min(n, 65536))
            except OSError:
                return -1
            ctypes.memmove(buf, data, len(data))
            return len(data)

        def rnd(buf, n):
            ctypes.memmove(buf, (random_fn or os.urandom)(n), n)

        self._cbs = (SENDFN(send), RECVFN(recv), RANDFN(rnd))
        self.io = Io(self._cbs[0], self._cbs[1], None)
        self.cfg = Config(server_name.encode() if server_name else None, alpn.encode() if alpn else None,
                          ctx.h if ctx else None, flags, suites, int(now if now is not None else time.time()), self._cbs[2])
        self.c = lib().tls_new(ctypes.byref(self.io), ctypes.byref(self.cfg))
        assert self.c

    def handshake(self):
        self.error = lib().tls_handshake(self.c)
        return self.error

    @property
    def info(self):
        return lib().tls_get_info(self.c).contents

    def alert(self):
        a = lib().tls_alert_received(self.c)
        return lib().tls_alert_name(a).decode() if a >= 0 else None

    def write(self, data):
        return lib().tls_write(self.c, data, len(data))

    def read(self, n=65536):
        """bytes, or an int error code; b'' after close_notify"""
        buf = ctypes.create_string_buffer(n)
        r = lib().tls_read(self.c, buf, n)
        return buf.raw[:r] if r >= 0 else r

    def read_all(self):
        out = b""
        while True:
            r = self.read()
            if isinstance(r, int):
                return out, r
            if not r:
                return out, 0
            out += r

    def close(self):
        lib().tls_shutdown(self.c)
        lib().tls_free(self.c)
        self.c = None
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------------------------------------------------ certificates

def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)


def new_key(kind="ec256"):
    if kind == "ec256":
        return ec.generate_private_key(ec.SECP256R1())
    if kind == "ec384":
        return ec.generate_private_key(ec.SECP384R1())
    if kind == "ec521":
        return ec.generate_private_key(ec.SECP521R1())
    if kind.startswith("rsa"):
        return rsa.generate_private_key(65537, int(kind[3:]))
    raise ValueError(kind)


def name(cn, org="TouhouOS Test"):
    return x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, org), x509.NameAttribute(NameOID.COMMON_NAME, cn)])


class Cert:
    def __init__(self, key, cert):
        self.key, self.cert = key, cert

    @property
    def pem(self):
        return self.cert.public_bytes(serialization.Encoding.PEM).decode()

    @property
    def der(self):
        return self.cert.public_bytes(serialization.Encoding.DER)

    @property
    def key_pem(self):
        return self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()).decode()


def issue(cn, key, issuer=None, sans=None, ca=False, path_len=None, not_before=None, not_after=None, hash_alg=None, eku="server",
          key_usage="auto", ski=True, extra_ext=()):
    """Makes a certificate for `key`, signed by `issuer` (a Cert) or, if there is none, by itself."""
    now = now_utc()
    nb = not_before or now - datetime.timedelta(days=1)
    na = not_after or now + datetime.timedelta(days=30)
    sign_key = issuer.key if issuer else key
    issuer_name = issuer.cert.subject if issuer else name(cn)
    if hash_alg is None:
        hash_alg = hashes.SHA384 if isinstance(sign_key, ec.EllipticCurvePrivateKey) and sign_key.curve.key_size == 384 else hashes.SHA256
    b = (x509.CertificateBuilder().subject_name(name(cn)).issuer_name(issuer_name).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(nb).not_valid_after(na))
    b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=path_len if ca else None), critical=True)
    if key_usage == "auto":
        if ca:
            b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
        elif isinstance(key, rsa.RSAPrivateKey):
            b = b.add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
        else:
            b = b.add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
    if ski:
        b = b.add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        if issuer:
            b = b.add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer.key.public_key()), critical=False)
    if sans:
        import ipaddress
        gn = []
        for s in sans:
            try:
                gn.append(x509.IPAddress(ipaddress.ip_address(s)))
            except ValueError:
                gn.append(x509.DNSName(s))
        b = b.add_extension(x509.SubjectAlternativeName(gn), critical=False)
    if eku == "server":
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    elif eku == "client":
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
    for ext, critical in extra_ext:
        b = b.add_extension(ext, critical=critical)
    return Cert(key, b.sign(sign_key, hash_alg()))


class Files:
    """Temporary PEM files for ssl.SSLContext.load_cert_chain."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="tlstest-")

    def write(self, fname, text):
        p = Path(self.dir) / fname
        p.write_text(text)
        return str(p)


_files = Files()


def server_context(leaf, chain=(), min_version=None, max_version=None, ciphers=None, curve=None, alpn=None):
    """An OpenSSL server context for `leaf` (a Cert) followed by the intermediates in `chain`."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    n = str(len(os.listdir(_files.dir)))
    certfile = _files.write(f"c{n}.pem", leaf.pem + "".join(c.pem for c in chain))
    keyfile = _files.write(f"k{n}.pem", leaf.key_pem)
    ctx.load_cert_chain(certfile, keyfile)
    if min_version:
        ctx.minimum_version = min_version
    if max_version:
        ctx.maximum_version = max_version
    if ciphers:
        ctx.set_ciphers(ciphers)
    if curve:
        ctx.set_ecdh_curve(curve)
    if alpn:
        ctx.set_alpn_protocols(alpn)
    return ctx


def http_handler(body=b"hello from OpenSSL\n"):
    def handle(s):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                return
            data += chunk
        s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
        try:
            s.unwrap()
        except (ssl.SSLError, OSError):
            pass
    return handle


def echo_handler(s):
    while True:
        data = s.recv(65536)
        if not data:
            break
        s.sendall(data)
    try:
        s.unwrap()
    except (ssl.SSLError, OSError):
        pass


class Server(threading.Thread):
    """Accepts connections on 127.0.0.1 (random port), wraps them with `ctx` and hands them to handler(tls_socket)."""

    def __init__(self, ctx, handler=None, raw_handler=None):
        super().__init__(daemon=True)
        self.ctx = ctx
        self.handler = handler or http_handler()
        self.raw_handler = raw_handler
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.errors = []
        self.stop_flag = False
        self.start()

    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop_flag:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            conn.settimeout(30)
            if self.raw_handler:
                try:
                    self.raw_handler(conn)
                except Exception as e:  # noqa: BLE001
                    self.errors.append(e)
                continue
            try:
                s = self.ctx.wrap_socket(conn, server_side=True)
            except (ssl.SSLError, OSError) as e:
                self.errors.append(e)
                conn.close()
                continue
            try:
                self.handler(s)
            except (ssl.SSLError, OSError) as e:
                self.errors.append(e)
            finally:
                try:
                    s.close()
                except OSError:
                    pass

    def stop(self):
        self.stop_flag = True
        try:
            self.sock.close()
        except OSError:
            pass
        self.join(2)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def connect(server, **kw):
    s = socket.create_connection(("127.0.0.1", server.port), timeout=30)
    return Client(s, **kw)


def fetch(server, **kw):
    """One request over TLS; returns (handshake_result, response_bytes, client)."""
    c = connect(server, **kw)
    rc = c.handshake()
    if rc:
        return rc, b"", c
    c.write(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    data, _ = c.read_all()
    return 0, data, c


# ------------------------------------------------------------------------------------------------------------ fault injection

def _read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class Proxy(threading.Thread):
    """A TCP proxy that understands TLS record framing. mutate(direction, index, record) -> list of byte strings to forward
    (direction 'c2s' or 's2c', index counts the records seen in that direction, record includes its 5-byte header);
    raise EOFError from it to close both sides at that point."""

    def __init__(self, target_port, mutate=None):
        super().__init__(daemon=True)
        self.target_port = target_port
        self.mutate = mutate or (lambda d, i, r: [r])
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.stop_flag = False
        self.log = []
        self.start()

    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop_flag:
            try:
                client, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                server = socket.create_connection(("127.0.0.1", self.target_port), timeout=30)
            except OSError:
                client.close()
                continue
            client.settimeout(30)
            for direction, src, dst in (("c2s", client, server), ("s2c", server, client)):
                threading.Thread(target=self._pump, args=(direction, src, dst, client, server), daemon=True).start()

    def _pump(self, direction, src, dst, client, server):
        index = 0
        try:
            while True:
                hdr = _read_exact(src, 5)
                if hdr is None:
                    break
                body = _read_exact(src, int.from_bytes(hdr[3:5], "big"))
                if body is None:
                    break
                self.log.append((direction, index, hdr[0], len(body)))
                for chunk in self.mutate(direction, index, hdr + body):
                    dst.sendall(chunk)
                index += 1
        except (EOFError, OSError):
            pass
        finally:
            for s in (client, server):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    s.close()
                except OSError:
                    pass

    def stop(self):
        self.stop_flag = True
        try:
            self.sock.close()
        except OSError:
            pass
        self.join(2)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
