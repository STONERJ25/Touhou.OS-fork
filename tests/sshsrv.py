"""A small SSH-2 server for testing the ssh client (docs/SSH_CLIENT.md), written in Python with the `cryptography` package.

It exists for two reasons. The OS has its own sshd, but a client and a server written by the same hand can share a misreading of the protocol; this one was written
separately, from the RFCs and OpenSSH's PROTOCOL files, and it is checked against the real OpenSSH client (tests/host/test_sshclient.py runs `ssh` against it too).
And the OS's sshd has only public keys: the password, keyboard-interactive, banner, re-keying and misbehaviour paths of the client need a server that has them.

What it speaks: key exchange curve25519-sha256 (and the @libssh.org name) with OpenSSH's strict key exchange, host key ssh-ed25519, cipher
chacha20-poly1305@openssh.com, no compression. Authentication: none, password, publickey, keyboard-interactive. Channels: session (exec, shell, pty-req,
window-change, subsystem "sftp"), direct-tcpip (a relay to a TCP port on this PC, for ssh -L and -D) and tcpip-forward (for ssh -R). Global requests are answered.

The exec commands are a toy set that make the client's behaviour visible (see Exec below); `shell` is a toy shell with a prompt. Everything the client did is
recorded in Server.events, so a test can ask "did the server see a window-change of 100x30".

Knobs for misbehaviour (Server arguments): strict, kex_algs / hostkey_algs / ciphers (lists offered), rekey_every (re-key after this many bytes), window and
max_packet (what the server offers the client), banner, bad_signature, ignore_before_kexinit (the Terrapin shape), disconnect_after_auth.

Run it by hand:  python tests/sshsrv.py --port 2222 --user alice:secret --key alice=~/.ssh/id_ed25519.pub
"""
import argparse
import base64
import hashlib
import os
import select
import socket
import stat
import struct
import threading
import time
import traceback

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives.poly1305 import Poly1305

# message numbers
DISCONNECT, IGNORE, UNIMPLEMENTED, DEBUG, SERVICE_REQUEST, SERVICE_ACCEPT, EXT_INFO = 1, 2, 3, 4, 5, 6, 7
KEXINIT, NEWKEYS, KEX_ECDH_INIT, KEX_ECDH_REPLY = 20, 21, 30, 31
USERAUTH_REQUEST, USERAUTH_FAILURE, USERAUTH_SUCCESS, USERAUTH_BANNER, USERAUTH_PK_OK = 50, 51, 52, 53, 60
USERAUTH_INFO_REQUEST, USERAUTH_INFO_RESPONSE = 60, 61
GLOBAL_REQUEST, REQUEST_SUCCESS, REQUEST_FAILURE = 80, 81, 82
CHANNEL_OPEN, CHANNEL_OPEN_CONFIRMATION, CHANNEL_OPEN_FAILURE, CHANNEL_WINDOW_ADJUST = 90, 91, 92, 93
CHANNEL_DATA, CHANNEL_EXTENDED_DATA, CHANNEL_EOF, CHANNEL_CLOSE, CHANNEL_REQUEST, CHANNEL_SUCCESS, CHANNEL_FAILURE = 94, 95, 96, 97, 98, 99, 100


def sstr(b):
    if isinstance(b, str):
        b = b.encode()
    return struct.pack(">I", len(b)) + b


def u32(n):
    return struct.pack(">I", n)


def mpint(n):
    if n == 0:
        return u32(0)
    b = n.to_bytes((n.bit_length() + 8) // 8, "big")
    return sstr(b)


class Reader:
    def __init__(self, data):
        self.b, self.i = bytes(data), 0

    def take(self, n):
        if self.i + n > len(self.b):
            raise ValueError("short message")
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def string(self):
        return self.take(self.u32())

    def boolean(self):
        return self.u8() != 0

    def rest(self):
        v = self.b[self.i:]
        self.i = len(self.b)
        return v


def ed25519_blob(pub32):
    return sstr("ssh-ed25519") + sstr(pub32)


def fingerprint(blob):
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


class Closed(Exception):
    pass


def chacha(key, seq, counter):
    """OpenSSH's ChaCha20: a 64-bit block counter and a 64-bit nonce (the packet's sequence number), as the 16-byte IV the cryptography package takes."""
    iv = struct.pack("<I", counter) + b"\0\0\0\0" + struct.pack(">Q", seq)
    return Cipher(algorithms.ChaCha20(key, iv), mode=None).encryptor()


class Channel:
    def __init__(self, conn, local, remote, window, maxpkt):
        self.conn, self.local, self.remote = conn, local, remote
        self.peer_window, self.peer_maxpkt = window, maxpkt
        self.our_window = conn.srv.window
        self.kind = "session"
        self.pty = None
        self.size = (80, 24)
        self.outq = bytearray()
        self.eof_pending = False
        self.exit_status = None
        self.exit_signal = None
        self.close_pending = False
        self.sent_eof = self.sent_close = self.got_close = False
        self.handler = None
        self.lock = threading.Lock()
        self.pending_adjust = 0
        self.sock = None                         # direct-tcpip: the relay socket

    def send(self, data):
        with self.lock:
            self.outq += data
        self.conn.wake()

    def finish(self, status=0, signal=None):
        with self.lock:
            self.exit_status, self.exit_signal = status, signal
            self.eof_pending = True
            self.close_pending = True
        self.conn.wake()

    def close_soon(self):
        with self.lock:
            self.eof_pending = True
            self.close_pending = True
        self.conn.wake()


class Exec:
    """The toy commands. Each is a handler with start(ch), data(ch, bytes), eof(ch). `echo` and `status` are enough for most tests."""

    def __init__(self, cmd):
        self.cmd = cmd
        self.parts = cmd.split()
        self.count = 0
        self.h = hashlib.sha256()

    def start(self, ch):
        p = self.parts
        name = p[0] if p else ""
        if name == "echo":
            ch.send((" ".join(p[1:]) + "\n").encode())
            ch.finish(0)
        elif name == "status":
            ch.finish(int(p[1]))
        elif name == "stderr":
            ch.conn.send_ext(ch, (" ".join(p[1:]) + "\n").encode())
            ch.finish(0)
        elif name == "both":
            ch.send(b"to-out\n")
            ch.conn.send_ext(ch, b"to-err\n")
            ch.finish(4)
        elif name == "gen":                       # gen N: N bytes of a pattern the test can recompute
            n = int(p[1])
            ch.send(bytes((i * 7 + 3) & 255 for i in range(n)))
            ch.finish(0)
        elif name == "signal":                    # dies of a signal: exit-signal instead of exit-status
            ch.finish(None, p[1] if len(p) > 1 else "TERM")
        elif name == "env":
            ch.send(f"pty={ch.pty!r} size={ch.size}\n".encode())
            ch.finish(0)
        elif name == "rekey":                     # output, a key exchange the server starts, more output
            ch.send(b"before-rekey\n")
            ch.conn.want_rekey = True
            ch.conn.on_rekey.append(lambda: (ch.send(b"after-rekey\n"), ch.finish(0)))
        elif name == "noexit":                    # closes the channel without ever sending an exit status
            ch.send(b"no status\n")
            ch.close_soon()
        elif name in ("cat", "sum", "upper", "hold"):
            pass                                  # these wait for input
        elif name == "sleep":
            threading.Timer(float(p[1]), lambda: ch.finish(0)).start()
        elif name == "banner-first":
            ch.send(b"x\n")
            ch.finish(0)
        else:
            ch.conn.send_ext(ch, f"unknown command: {self.cmd}\n".encode())
            ch.finish(127)

    def data(self, ch, data):
        name = self.parts[0] if self.parts else ""
        if name == "cat":
            ch.send(data)
        elif name == "upper":
            ch.send(data.upper())
        elif name == "sum":
            self.h.update(data)
            self.count += len(data)

    def eof(self, ch):
        name = self.parts[0] if self.parts else ""
        if name == "cat" or name == "upper":
            ch.finish(0)
        elif name == "sum":
            ch.send(f"{self.count} {self.h.hexdigest()}\n".encode())
            ch.finish(0)


class Shell:
    """A toy interactive shell. On a pty it echoes what is typed (a real pty does that), turns CR into CR LF and understands backspace."""

    PROMPT = b"srv$ "

    def __init__(self):
        self.line = bytearray()

    def start(self, ch):
        ch.send(b"Welcome to the test server\r\n" + self.PROMPT)

    def data(self, ch, data):
        for b in data:
            if b in (13, 10):
                ch.send(b"\r\n" if ch.pty else b"")
                self.run(ch, self.line.decode("utf-8", "replace"))
                self.line = bytearray()
            elif b in (8, 127):
                if self.line:
                    self.line.pop()
                    if ch.pty:
                        ch.send(b"\b \b")
            elif b == 3:
                self.line = bytearray()
                ch.send(b"^C\r\n" + self.PROMPT)
            elif b == 4 and not self.line:
                ch.finish(0)
            else:
                self.line.append(b)
                if ch.pty:
                    ch.send(bytes([b]))

    def run(self, ch, line):
        parts = line.split()
        out = ""
        if not parts:
            pass
        elif parts[0] == "echo":
            out = " ".join(parts[1:])
        elif parts[0] == "size":
            out = "size=%dx%d" % ch.size
        elif parts[0] == "term":
            out = "term=%s" % (ch.pty[0] if ch.pty else "none")
        elif parts[0] == "exit":
            ch.send(b"logout\r\n")
            ch.finish(int(parts[1]) if len(parts) > 1 else 0)
            return
        elif parts[0] == "stderr":
            ch.conn.send_ext(ch, (" ".join(parts[1:]) + "\r\n").encode())
        elif parts[0] == "big":
            ch.send(bytes((i % 26) + 97 for i in range(int(parts[1]))))
            out = ""
        else:
            out = "unknown: " + parts[0]
        ch.send((out + "\r\n").encode() if out else b"")
        ch.send(self.PROMPT)

    def eof(self, ch):
        ch.finish(0)


class Server:
    def __init__(self, host="127.0.0.1", port=0, host_key=None, users=None, keys=None, kbdint=None, methods=None, strict=True, kex_algs=None, hostkey_algs=None,
                 ciphers=None, rekey_every=0, window=1 << 20, max_packet=32768, banner=None, bad_signature=False, ignore_before_kexinit=False, max_tries=6,
                 allow_none=False, forward_ok=True, sftp_root=None, disconnect_after_auth=False, keepalive_requests=0):
        self.host_key = host_key or Ed25519PrivateKey.generate()
        self.host_pub = self.host_key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.host_blob = ed25519_blob(self.host_pub)
        self.users = users or {}                        # name -> password
        self.keys = keys or {}                          # name -> list of 32-byte ed25519 public keys
        self.kbdint = kbdint or {}                      # name -> list of (prompt, echo, answer)
        self.methods = methods or [m for m, have in (("publickey", self.keys), ("keyboard-interactive", self.kbdint), ("password", self.users)) if have]
        self.strict, self.rekey_every, self.window, self.max_packet = strict, rekey_every, window, max_packet
        self.kex_algs = kex_algs or ["curve25519-sha256", "curve25519-sha256@libssh.org"]
        self.hostkey_algs = hostkey_algs or ["ssh-ed25519"]
        self.ciphers = ciphers or ["chacha20-poly1305@openssh.com"]
        self.banner, self.bad_signature, self.ignore_before_kexinit = banner, bad_signature, ignore_before_kexinit
        self.max_tries, self.allow_none, self.forward_ok, self.sftp_root = max_tries, allow_none, forward_ok, sftp_root
        self.disconnect_after_auth, self.keepalive_requests = disconnect_after_auth, keepalive_requests
        self.events = []
        self.frozen = False
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.host = host
        self.conns = []
        self.stopping = False
        threading.Thread(target=self._accept, daemon=True).start()

    @property
    def fingerprint(self):
        return fingerprint(self.host_blob)

    def public_line(self, comment=""):
        return "ssh-ed25519 " + base64.b64encode(self.host_blob).decode() + (" " + comment if comment else "")

    def log(self, *event):
        with self.lock:
            self.events.append(event)

    def find(self, kind):
        with self.lock:
            return [e for e in self.events if e[0] == kind]

    def wait_for(self, kind, timeout=20, count=1):
        end = time.time() + timeout
        while time.time() < end:
            if len(self.find(kind)) >= count:
                return self.find(kind)
            time.sleep(0.05)
        raise AssertionError(f"the server never saw {kind!r}; events: {self.events}")

    def _accept(self):
        while not self.stopping:
            try:
                s, addr = self.sock.accept()
            except OSError:
                return
            c = Conn(self, s, addr)
            self.conns.append(c)
            threading.Thread(target=c.run, daemon=True).start()

    def stop(self):
        self.stopping = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)                  # wakes the thread blocked in accept(), which would otherwise hold the port
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        for c in list(self.conns):
            c.abort()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


class Conn:
    def __init__(self, srv, sock, addr):
        self.srv, self.sock, self.addr = srv, sock, addr
        self.seq_in = self.seq_out = 0
        self.enc_in = self.enc_out = False
        self.keys_in = self.keys_out = None
        self.strict = False
        self.session_id = None
        self.channels = {}
        self.next_id = 0
        self.user = None
        self.authed = False
        self.tries = 0
        self.wake_r, self.wake_w = socket.socketpair()
        self.want_rekey = False
        self.bytes = 0
        self.kexing = False
        self.deferred = []
        self.on_rekey = []
        self.kex_count = 0
        self.dead = False
        self.client_id = b""
        self.listeners = {}
        self.kbd_state = None
        self.sftp = {}

    # ---- plumbing
    def abort(self):
        self.dead = True
        for s in (self.sock, self.wake_w):
            try:
                s.close()
            except OSError:
                pass

    def wake(self):
        try:
            self.wake_w.send(b"x")
        except OSError:
            pass

    def _recvn(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise Closed()
            buf += chunk
        return buf

    def recv_packet(self):
        if self.enc_in:
            main, hdr = self.keys_in
            enc_len = self._recvn(4)
            length = struct.unpack(">I", chacha(hdr, self.seq_in, 0).update(enc_len))[0]
            if length < 8 or length > 262144 or length % 8:
                raise ValueError("bad packet length %d" % length)
            body = self._recvn(length + 16)
            pk = chacha(main, self.seq_in, 0).update(bytes(32))
            tag = Poly1305.generate_tag(pk, enc_len + body[:-16])
            if tag != body[-16:]:
                raise ValueError("bad MAC from the client")
            plain = chacha(main, self.seq_in, 1).update(body[:-16])
            pad = plain[0]
            payload = plain[1:length - pad]
            self.bytes += 4 + length + 16
        else:
            length = struct.unpack(">I", self._recvn(4))[0]
            if length < 12 or length > 262144:
                raise ValueError("bad packet length %d" % length)
            body = self._recvn(length)
            payload = body[1:length - body[0]]
        self.seq_in += 1
        return payload

    def send_packet(self, payload, raw=False):
        if self.kexing and not raw and payload[0] > 49:
            self.deferred.append(payload)               # nothing but key exchange messages while keys are changing
            return
        pad = 8 - ((1 + len(payload)) % 8 if self.enc_out else (5 + len(payload)) % 8)
        if pad < 4:
            pad += 8
        body = bytes([pad]) + payload + os.urandom(pad)
        if self.enc_out:
            main, hdr = self.keys_out
            enc_len = chacha(hdr, self.seq_out, 0).update(struct.pack(">I", len(body)))
            enc_body = chacha(main, self.seq_out, 1).update(body)
            pk = chacha(main, self.seq_out, 0).update(bytes(32))
            out = enc_len + enc_body + Poly1305.generate_tag(pk, enc_len + enc_body)
            self.bytes += len(out)
        else:
            out = struct.pack(">I", len(body)) + body
        self.seq_out += 1
        self.sock.sendall(out)

    # ---- key exchange
    def kexinit_payload(self):
        srv = self.srv
        kex = list(srv.kex_algs)
        if srv.strict and not self.kex_count:
            kex.append("kex-strict-s-v00@openssh.com")
        nl = lambda items: sstr(",".join(items))    # noqa: E731
        return (bytes([KEXINIT]) + os.urandom(16) + nl(kex) + nl(srv.hostkey_algs) + nl(srv.ciphers) * 2 + nl(["hmac-sha2-256"]) * 2 + nl(["none"]) * 2 +
                nl([]) * 2 + b"\0" + u32(0))

    def do_kex(self, their_kexinit, ours):
        srv = self.srv
        if not self.kex_count:
            their = Reader(their_kexinit[1 + 16:])
            kex_c = their.string().decode().split(",")
            self.strict = srv.strict and "kex-strict-c-v00@openssh.com" in kex_c
            if self.strict and self.seq_in != 1:
                raise ValueError("strict KEX: the client's KEXINIT was not its first packet")
        self.kexing = True
        r = Reader(self.recv_packet())
        if r.u8() != KEX_ECDH_INIT:
            raise ValueError("expected KEX_ECDH_INIT")
        qc = r.string()
        eph = X25519PrivateKey.generate()
        qs = eph.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        k = int.from_bytes(eph.exchange(X25519PublicKey.from_public_bytes(qc)), "big")
        h = hashlib.sha256()
        for item in (self.client_id, self.server_id, their_kexinit, ours, srv.host_blob, qc, qs):
            h.update(sstr(item))
        h.update(mpint(k))
        H = h.digest()
        if self.session_id is None:
            self.session_id = H
        sig = srv.host_key.sign(H)
        if srv.bad_signature:
            sig = bytes([sig[0] ^ 1]) + sig[1:]
        self.send_packet(bytes([KEX_ECDH_REPLY]) + sstr(srv.host_blob) + sstr(qs) + sstr(sstr("ssh-ed25519") + sstr(sig)), raw=True)

        def derive(letter):
            first = hashlib.sha256(mpint(k) + H + letter + self.session_id).digest()
            return first + hashlib.sha256(mpint(k) + H + first).digest()
        c2s, s2c = derive(b"C"), derive(b"D")
        self.send_packet(bytes([NEWKEYS]), raw=True)
        self.keys_out = (s2c[:32], s2c[32:])
        self.enc_out = True
        if self.strict:
            self.seq_out = 0
        r = self.recv_packet()
        if r != bytes([NEWKEYS]):
            raise ValueError("expected NEWKEYS")
        self.keys_in = (c2s[:32], c2s[32:])
        self.enc_in = True
        if self.strict:
            self.seq_in = 0
        self.kex_count += 1
        self.bytes = 0
        self.kexing = False
        srv.log("kex", self.kex_count)
        for payload in self.deferred:
            self.send_packet(payload)
        self.deferred = []
        callbacks, self.on_rekey = self.on_rekey, []
        if self.kex_count > 1:
            for cb in callbacks:
                cb()
        else:
            self.on_rekey = callbacks

    def start_kex(self, their=None):
        """Our KEXINIT out, theirs in (read here unless given), then the exchange."""
        ours = self.kexinit_payload()
        self.send_packet(ours, raw=True)
        if their is None:
            self.kexing = True                           # from our KEXINIT on, only key exchange messages go out; what the client had already sent is handled
            their = self.recv_packet()
            while their[0] != KEXINIT:
                if self.kex_count == 0:
                    raise ValueError("expected KEXINIT, got %d" % their[0])
                if not self.handle(their):
                    return
                their = self.recv_packet()
        self.do_kex(their, ours)

    # ---- the connection
    def run(self):
        srv = self.srv
        try:
            self.server_id = b"SSH-2.0-PyTestSrv_1"
            self.sock.sendall(self.server_id + b"\r\n")
            if srv.ignore_before_kexinit:
                self.send_packet(bytes([IGNORE]) + sstr(b"terrapin"), raw=True)
            line = b""
            while True:
                b = self.sock.recv(1)
                if not b:
                    raise Closed()
                if b == b"\n":
                    if line.startswith(b"SSH-"):
                        break
                    line = b""
                else:
                    line += b
            self.client_id = line.rstrip(b"\r")
            srv.log("connect", self.client_id.decode(errors="replace"))
            self.start_kex()
            self.loop()
        except (Closed, ConnectionError, OSError):
            pass
        except Exception:                                # a bug in the server or a protocol violation by the client: tests want to see which
            srv.log("error", traceback.format_exc())
        finally:
            srv.log("closed")
            self.abort()

    def loop(self):
        srv = self.srv
        while not self.dead:
            if srv.frozen:                                   # a server that stopped answering (a machine that hung): reads nothing, says nothing
                time.sleep(0.05)
                continue
            self.pump_channels()
            if self.want_rekey or (srv.rekey_every and self.bytes > srv.rekey_every and self.authed):
                self.want_rekey = False
                self.start_kex()
                continue
            busy = any(ch.outq and ch.peer_window > 0 for ch in self.channels.values())
            readable, _, _ = select.select([self.sock, self.wake_r], [], [], 0 if busy else 0.5)
            if self.wake_r in readable:
                try:
                    self.wake_r.recv(4096)
                except OSError:
                    pass
            if self.sock in readable:
                payload = self.recv_packet()
                if not self.handle(payload):
                    return

    def pump_channels(self):
        for ch in list(self.channels.values()):
            self.pump(ch)

    def pump(self, ch):
        if ch.sent_close:
            return
        with ch.lock:
            while ch.outq and ch.peer_window > 0:
                if self.srv.rekey_every and self.bytes > self.srv.rekey_every and not self.kexing:
                    break                                   # the main loop starts the key exchange; the rest goes out afterwards
                n = min(len(ch.outq), ch.peer_window, ch.peer_maxpkt, 16384)
                data = bytes(ch.outq[:n])
                del ch.outq[:n]
                ch.peer_window -= n
                self.send_packet(bytes([CHANNEL_DATA]) + u32(ch.remote) + sstr(data))
            drained = not ch.outq
            eof, close = ch.eof_pending, ch.close_pending
        if drained and eof and not ch.sent_eof:
            if ch.exit_signal is not None:
                self.send_packet(bytes([CHANNEL_REQUEST]) + u32(ch.remote) + sstr("exit-signal") + b"\0" + sstr(ch.exit_signal) + b"\0" + sstr("") + sstr(""))
            elif ch.exit_status is not None:
                self.send_packet(bytes([CHANNEL_REQUEST]) + u32(ch.remote) + sstr("exit-status") + b"\0" + u32(ch.exit_status))
            self.send_packet(bytes([CHANNEL_EOF]) + u32(ch.remote))
            ch.sent_eof = True
        if drained and close and ch.sent_eof and not ch.sent_close:
            self.send_packet(bytes([CHANNEL_CLOSE]) + u32(ch.remote))
            ch.sent_close = True
            if ch.got_close:
                self.channels.pop(ch.local, None)

    def send_ext(self, ch, data):
        for i in range(0, len(data), 16384):
            self.send_packet(bytes([CHANNEL_EXTENDED_DATA]) + u32(ch.remote) + u32(1) + sstr(data[i:i + 16384]))

    def fail_auth(self):
        self.send_packet(bytes([USERAUTH_FAILURE]) + sstr(",".join(self.srv.methods)) + b"\0")

    def handle(self, p):
        srv = self.srv
        t = p[0]
        r = Reader(p[1:])
        if t == DISCONNECT:
            srv.log("disconnect", r.u32(), r.string().decode(errors="replace"))
            return False
        if t in (IGNORE, DEBUG, UNIMPLEMENTED, EXT_INFO):
            return True
        if t == KEXINIT:
            self.start_kex(their=p)
            return True
        if t == SERVICE_REQUEST:
            name = r.string()
            if name == b"ssh-userauth":
                self.send_packet(bytes([SERVICE_ACCEPT]) + sstr(name))
                if srv.banner:
                    self.send_packet(bytes([USERAUTH_BANNER]) + sstr(srv.banner) + sstr(""))
            else:
                self.send_packet(bytes([DISCONNECT]) + u32(7) + sstr("no such service") + sstr(""))
                return False
            return True
        if t == USERAUTH_REQUEST:
            return self.userauth(r)
        if t == USERAUTH_INFO_RESPONSE:
            return self.kbd_response(r)
        if not self.authed:
            self.send_packet(bytes([DISCONNECT]) + u32(2) + sstr("not authenticated") + sstr(""))
            return False
        if t == GLOBAL_REQUEST:
            return self.global_request(r)
        if t == CHANNEL_OPEN:
            return self.channel_open(r)
        if t in (CHANNEL_OPEN_CONFIRMATION, CHANNEL_OPEN_FAILURE, CHANNEL_DATA, CHANNEL_EXTENDED_DATA, CHANNEL_EOF, CHANNEL_CLOSE, CHANNEL_WINDOW_ADJUST, CHANNEL_REQUEST,
                 CHANNEL_SUCCESS, CHANNEL_FAILURE):
            return self.channel_message(t, r)
        if t in (REQUEST_SUCCESS, REQUEST_FAILURE):
            return True
        self.send_packet(bytes([UNIMPLEMENTED]) + u32(self.seq_in - 1))
        return True

    # ---- authentication
    def userauth(self, r):
        srv = self.srv
        user, service, method = r.string().decode(), r.string(), r.string().decode()
        ok = False
        if service != b"ssh-connection":
            self.send_packet(bytes([DISCONNECT]) + u32(7) + sstr("bad service") + sstr(""))
            return False
        if method == "none":
            ok = srv.allow_none
            srv.log("auth", user, "none", ok)
            if not ok:
                self.fail_auth()
                return True
        elif method == "password" and "password" in srv.methods:
            r.boolean()
            pw = r.string().decode()
            ok = srv.users.get(user) == pw
            srv.log("auth", user, "password", ok)
        elif method == "publickey" and "publickey" in srv.methods:
            has_sig = r.boolean()
            alg, blob = r.string(), r.string()
            keys = [ed25519_blob(k) for k in srv.keys.get(user, [])]
            listed = alg == b"ssh-ed25519" and blob in keys
            if not has_sig:
                srv.log("auth-query", user, "publickey", listed)
                if listed:
                    self.send_packet(bytes([USERAUTH_PK_OK]) + sstr(alg) + sstr(blob))
                    return True
            else:
                sig = Reader(r.string())
                if sig.string() != b"ssh-ed25519":
                    listed = False
                sigbytes = sig.string()
                signed = sstr(self.session_id) + bytes([USERAUTH_REQUEST]) + sstr(user) + sstr(service) + sstr(method) + b"\1" + sstr(alg) + sstr(blob)
                try:
                    kb = Reader(blob)
                    kb.string()
                    Ed25519PublicKey.from_public_bytes(kb.string()).verify(sigbytes, signed)
                    ok = listed
                except (InvalidSignature, ValueError):
                    ok = False
                srv.log("auth", user, "publickey", ok)
        elif method == "keyboard-interactive" and "keyboard-interactive" in srv.methods and user in srv.kbdint:
            self.kbd_state = (user, 0)
            prompts = srv.kbdint[user]
            srv.log("auth-query", user, "keyboard-interactive", True)
            self.send_packet(bytes([USERAUTH_INFO_REQUEST]) + sstr("test") + sstr("answer the questions") + sstr("") + u32(len(prompts)) +
                             b"".join(sstr(p) + (b"\1" if e else b"\0") for p, e, _ in prompts))
            return True
        else:
            srv.log("auth", user, method, False)
        if ok:
            self.user, self.authed = user, True
            self.send_packet(bytes([USERAUTH_SUCCESS]))
            if srv.disconnect_after_auth:
                self.send_packet(bytes([DISCONNECT]) + u32(11) + sstr("bye") + sstr(""))
                return False
            return True
        self.tries += 1
        if self.tries >= srv.max_tries:
            self.send_packet(bytes([DISCONNECT]) + u32(2) + sstr("Too many authentication failures") + sstr(""))
            return False
        self.fail_auth()
        return True

    def kbd_response(self, r):
        srv = self.srv
        if not self.kbd_state:
            return True
        user, _ = self.kbd_state
        n = r.u32()
        answers = [r.string().decode() for _ in range(n)]
        want = [a for _, _, a in srv.kbdint.get(user, [])]
        ok = answers == want
        srv.log("auth", user, "keyboard-interactive", ok, answers)
        self.kbd_state = None
        if ok:
            self.user, self.authed = user, True
            self.send_packet(bytes([USERAUTH_SUCCESS]))
            return True
        self.tries += 1
        if self.tries >= srv.max_tries:
            self.send_packet(bytes([DISCONNECT]) + u32(2) + sstr("Too many authentication failures") + sstr(""))
            return False
        self.fail_auth()
        return True

    # ---- channels
    def global_request(self, r):
        name, want = r.string().decode(), r.boolean()
        self.srv.log("global", name)
        if name == "tcpip-forward":
            host, port = r.string().decode(), r.u32()
            ok = self.srv.forward_ok and self.open_listener(host, port)
            if want:
                self.send_packet(bytes([REQUEST_SUCCESS if ok else REQUEST_FAILURE]) + (u32(self.listeners[(host, port)][1]) if ok and port == 0 else b""))
        elif want:
            self.send_packet(bytes([REQUEST_FAILURE]))
        return True

    def open_listener(self, host, port):
        try:
            ls = socket.socket()
            ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            ls.bind(("127.0.0.1", port))
            ls.listen(8)
        except OSError:
            return False
        actual = ls.getsockname()[1]
        self.listeners[(host, port)] = (ls, actual)

        def accept_loop():
            while not self.dead:
                try:
                    s, addr = ls.accept()
                except OSError:
                    return
                self.reverse_open(s, host, actual, addr)
        threading.Thread(target=accept_loop, daemon=True).start()
        return True

    def reverse_open(self, s, host, port, addr):
        """A connection to a port opened with tcpip-forward: open a forwarded-tcpip channel to the client and relay."""
        ch = Channel(self, self.alloc_id(), None, 0, 0)
        ch.kind, ch.sock = "forwarded", s
        self.channels[ch.local] = ch
        self.send_packet(bytes([CHANNEL_OPEN]) + sstr("forwarded-tcpip") + u32(ch.local) + u32(self.srv.window) + u32(self.srv.max_packet) + sstr(host) + u32(port) +
                         sstr(addr[0]) + u32(addr[1]))
        self.wake()

    def alloc_id(self):
        n = self.next_id
        self.next_id += 1
        return n

    def start_relay(self, ch, sock):
        ch.sock = sock

        def reader():
            try:
                while True:
                    data = sock.recv(8192)
                    if not data:
                        break
                    ch.send(data)
            except OSError:
                pass
            ch.close_soon()
        threading.Thread(target=reader, daemon=True).start()

    def channel_open(self, r):
        srv = self.srv
        kind = r.string().decode()
        remote, window, maxpkt = r.u32(), r.u32(), r.u32()
        if kind == "session":
            ch = Channel(self, self.alloc_id(), remote, window, maxpkt)
            self.channels[ch.local] = ch
            srv.log("open", "session")
            self.send_packet(bytes([CHANNEL_OPEN_CONFIRMATION]) + u32(remote) + u32(ch.local) + u32(srv.window) + u32(srv.max_packet))
        elif kind == "direct-tcpip":
            host, port, ohost, oport = r.string().decode(), r.u32(), r.string().decode(), r.u32()
            srv.log("open", "direct-tcpip", host, port, ohost, oport)
            if not srv.forward_ok:
                self.send_packet(bytes([CHANNEL_OPEN_FAILURE]) + u32(remote) + u32(1) + sstr("forwarding is prohibited") + sstr(""))
                return True
            try:
                s = socket.create_connection((host, port), timeout=5)
            except OSError as e:
                self.send_packet(bytes([CHANNEL_OPEN_FAILURE]) + u32(remote) + u32(2) + sstr("connect failed: %s" % e) + sstr(""))
                return True
            s.settimeout(None)
            ch = Channel(self, self.alloc_id(), remote, window, maxpkt)
            ch.kind = "direct"
            self.channels[ch.local] = ch
            self.send_packet(bytes([CHANNEL_OPEN_CONFIRMATION]) + u32(remote) + u32(ch.local) + u32(srv.window) + u32(srv.max_packet))
            self.start_relay(ch, s)
        else:
            self.send_packet(bytes([CHANNEL_OPEN_FAILURE]) + u32(remote) + u32(3) + sstr("unknown channel type") + sstr(""))
        return True

    def channel_message(self, t, r):
        srv = self.srv
        local = r.u32()
        ch = self.channels.get(local)
        if ch is None:
            return True
        if t == CHANNEL_OPEN_CONFIRMATION:                  # the client accepted a channel we opened (forwarded-tcpip): now there is a peer to relay to
            ch.remote, ch.peer_window, ch.peer_maxpkt = r.u32(), r.u32(), r.u32()
            srv.log("open-confirmed", ch.kind)
            if ch.sock:
                self.start_relay(ch, ch.sock)
            return True
        if t == CHANNEL_OPEN_FAILURE:
            srv.log("open-refused", ch.kind)
            if ch.sock:
                ch.sock.close()
            self.channels.pop(local, None)
            return True
        if t == CHANNEL_WINDOW_ADJUST:
            ch.peer_window += r.u32()
        elif t in (CHANNEL_DATA, CHANNEL_EXTENDED_DATA):
            if t == CHANNEL_EXTENDED_DATA:
                r.u32()
            data = r.string()
            if len(data) > ch.our_window:
                raise ValueError("the client overran the window")
            ch.our_window -= len(data)
            if ch.kind in ("direct", "forwarded") and ch.sock:
                try:
                    ch.sock.sendall(data)
                except OSError:
                    ch.close_soon()
            elif ch.handler:
                ch.handler.data(ch, data)
            ch.pending_adjust += len(data)
            if ch.pending_adjust >= srv.window // 2:
                self.send_packet(bytes([CHANNEL_WINDOW_ADJUST]) + u32(ch.remote) + u32(ch.pending_adjust))
                ch.our_window += ch.pending_adjust
                ch.pending_adjust = 0
        elif t == CHANNEL_EOF:
            if ch.kind in ("direct", "forwarded") and ch.sock:
                try:
                    ch.sock.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
            elif ch.handler:
                ch.handler.eof(ch)
            srv.log("eof", local)
        elif t == CHANNEL_CLOSE:
            ch.got_close = True
            if ch.sock:
                try:
                    ch.sock.close()
                except OSError:
                    pass
            if not ch.sent_close:
                ch.close_pending = ch.eof_pending = True
                ch.outq.clear()
                if ch.exit_status is None and ch.exit_signal is None and ch.handler is None:
                    pass
                self.pump(ch)
            if ch.sent_close:
                self.channels.pop(local, None)
            srv.log("close", local)
        elif t == CHANNEL_REQUEST:
            return self.channel_request(ch, r)
        return True

    def channel_request(self, ch, r):
        srv = self.srv
        kind, want = r.string().decode(), r.boolean()
        ok = False
        if kind == "pty-req":
            term, cols, rows, pw, ph, modes = r.string().decode(), r.u32(), r.u32(), r.u32(), r.u32(), r.string()
            ch.pty, ch.size = (term, modes), (cols, rows)
            srv.log("pty", term, cols, rows, modes)
            ok = True
        elif kind == "window-change":
            cols, rows = r.u32(), r.u32()
            ch.size = (cols, rows)
            srv.log("winch", cols, rows)
            if isinstance(ch.handler, Shell):
                pass
            return True
        elif kind == "env":
            srv.log("env", r.string().decode(), r.string().decode())
            ok = False
        elif kind in ("exec", "shell", "subsystem"):
            if kind == "exec":
                cmd = r.string().decode()
                srv.log("exec", cmd)
                ch.handler = Exec(cmd)
            elif kind == "shell":
                srv.log("shell", bool(ch.pty))
                ch.handler = Shell()
            else:
                name = r.string().decode()
                srv.log("subsystem", name)
                if name == "sftp" and srv.sftp_root is not None:
                    ch.handler = Sftp(srv.sftp_root)
                else:
                    ch.handler = None
            ok = ch.handler is not None
            if want:
                self.send_packet(bytes([CHANNEL_SUCCESS if ok else CHANNEL_FAILURE]) + u32(ch.remote))
                want = False
            if ok:
                ch.handler.start(ch)
        elif kind == "signal":
            srv.log("signal", r.string().decode())
            return True
        elif kind == "eow@openssh.com":
            return True
        if want:
            self.send_packet(bytes([CHANNEL_SUCCESS if ok else CHANNEL_FAILURE]) + u32(ch.remote))
        return True


class Sftp:
    """SFTP version 3 (draft-ietf-secsh-filexfer-02) on a directory of this PC: enough for the client's get, put, ls, mkdir, rm, rename, stat and chmod."""

    def __init__(self, root):
        self.root = os.path.realpath(root)
        self.buf = bytearray()
        self.handles = {}
        self.next = 1
        self.version_sent = False

    def start(self, ch):
        pass

    def eof(self, ch):
        ch.finish(0)

    def path(self, p):
        p = p.decode()
        full = os.path.normpath(os.path.join(self.root, p.lstrip("/")))              # not realpath: an operation on a symlink is an operation on the link
        if full != self.root and not full.startswith(self.root + os.sep):
            raise PermissionError(p)
        return full

    def data(self, ch, data):
        self.buf += data
        while len(self.buf) >= 4:
            n = struct.unpack(">I", self.buf[:4])[0]
            if len(self.buf) < 4 + n:
                return
            pkt = bytes(self.buf[4:4 + n])
            del self.buf[:4 + n]
            self.handle(ch, pkt)

    def reply(self, ch, payload):
        ch.send(u32(len(payload)) + payload)

    def status(self, ch, rid, code, msg=""):
        self.reply(ch, bytes([101]) + u32(rid) + u32(code) + sstr(msg) + sstr(""))

    def attrs(self, st):
        return u32(0xF) + struct.pack(">Q", st.st_size) + u32(st.st_uid) + u32(st.st_gid) + u32(st.st_mode) + u32(int(st.st_atime)) + u32(int(st.st_mtime))

    def handle(self, ch, pkt):
        t = pkt[0]
        r = Reader(pkt[1:])
        if t == 1:                                            # INIT
            self.reply(ch, bytes([2]) + u32(3))
            return
        rid = r.u32()
        try:
            if t in (7, 8, 17):                               # LSTAT, FSTAT, STAT
                if t == 8:
                    st = os.fstat(self.handles[r.string()][1])
                else:
                    p = self.path(r.string())
                    st = os.lstat(p) if t == 7 else os.stat(p)
                self.reply(ch, bytes([105]) + u32(rid) + self.attrs(st))
            elif t == 3:                                      # OPEN
                p, flags = self.path(r.string()), r.u32()
                fl = 0
                if flags & 1 and flags & 2:
                    fl = os.O_RDWR
                elif flags & 2:
                    fl = os.O_WRONLY
                else:
                    fl = os.O_RDONLY
                if flags & 8:
                    fl |= os.O_CREAT
                if flags & 0x10:
                    fl |= os.O_TRUNC
                if flags & 0x20:
                    fl |= os.O_EXCL
                fd = os.open(p, fl | getattr(os, "O_BINARY", 0), 0o644)
                h = b"h%d" % self.next
                self.next += 1
                self.handles[h] = ("file", fd)
                self.reply(ch, bytes([102]) + u32(rid) + sstr(h))
            elif t == 4:                                      # CLOSE
                h = r.string()
                kind, obj = self.handles.pop(h)
                if kind == "file":
                    os.close(obj)
                self.status(ch, rid, 0, "OK")
            elif t == 5:                                      # READ
                h, off, ln = r.string(), struct.unpack(">Q", r.take(8))[0], r.u32()
                fd = self.handles[h][1]
                os.lseek(fd, off, os.SEEK_SET)
                data = os.read(fd, min(ln, 262144))
                if not data:
                    self.status(ch, rid, 1, "EOF")
                else:
                    self.reply(ch, bytes([103]) + u32(rid) + sstr(data))
            elif t == 6:                                      # WRITE
                h, off, data = r.string(), struct.unpack(">Q", r.take(8))[0], r.string()
                fd = self.handles[h][1]
                os.lseek(fd, off, os.SEEK_SET)
                os.write(fd, data)
                self.status(ch, rid, 0, "OK")
            elif t == 11:                                     # OPENDIR
                p = self.path(r.string())
                names = sorted(os.listdir(p))
                h = b"d%d" % self.next
                self.next += 1
                self.handles[h] = ("dir", [(n, os.lstat(os.path.join(p, n))) for n in names])
                self.reply(ch, bytes([102]) + u32(rid) + sstr(h))
            elif t == 12:                                     # READDIR
                h = r.string()
                entries = self.handles[h][1]
                if not entries:
                    self.status(ch, rid, 1, "EOF")
                else:
                    batch, rest = entries[:50], entries[50:]
                    self.handles[h] = ("dir", rest)
                    body = u32(len(batch))
                    for name, st in batch:
                        long = "%s %3d %-8d %-8d %8d Jan  1 00:00 %s" % (stat.filemode(st.st_mode), st.st_nlink, st.st_uid, st.st_gid, st.st_size, name)
                        body += sstr(name) + sstr(long) + self.attrs(st)
                    self.reply(ch, bytes([104]) + u32(rid) + body)
            elif t == 13:                                     # REMOVE
                os.remove(self.path(r.string()))
                self.status(ch, rid, 0, "OK")
            elif t == 14:                                     # MKDIR
                os.mkdir(self.path(r.string()))
                self.status(ch, rid, 0, "OK")
            elif t == 15:                                     # RMDIR
                os.rmdir(self.path(r.string()))
                self.status(ch, rid, 0, "OK")
            elif t == 16:                                     # REALPATH
                p = r.string().decode()
                full = self.path(p.encode() if p else b"/")
                shown = "/" + os.path.relpath(full, self.root).replace(os.sep, "/")
                shown = "/" if shown == "/." else shown
                self.reply(ch, bytes([104]) + u32(rid) + u32(1) + sstr(shown) + sstr(shown) + u32(0))
            elif t == 18:                                     # RENAME
                os.rename(self.path(r.string()), self.path(r.string()))
                self.status(ch, rid, 0, "OK")
            elif t in (9, 10):                                # SETSTAT, FSETSTAT
                p = self.handles[r.string()][1] if t == 10 else self.path(r.string())
                flags = r.u32()
                if flags & 1:
                    r.take(8)
                if flags & 2:
                    r.u32(), r.u32()
                if flags & 4:
                    os.chmod(p, r.u32() & 0o7777)
                if flags & 8:
                    atime, mtime = r.u32(), r.u32()
                    os.utime(p, (atime, mtime))
                self.status(ch, rid, 0, "OK")
            elif t == 19:                                     # READLINK
                target = os.readlink(self.path(r.string()))
                self.reply(ch, bytes([104]) + u32(rid) + u32(1) + sstr(target) + sstr(target) + u32(0))
            elif t == 20:                                     # SYMLINK
                os.symlink(r.string().decode(), self.path(r.string()))
                self.status(ch, rid, 0, "OK")
            else:
                self.status(ch, rid, 8, "unsupported")
        except FileNotFoundError as e:
            self.status(ch, rid, 2, str(e))
        except PermissionError as e:
            self.status(ch, rid, 3, str(e))
        except (OSError, KeyError, ValueError) as e:
            self.status(ch, rid, 4, str(e))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=2222)
    ap.add_argument("--user", action="append", default=[], help="NAME:PASSWORD")
    ap.add_argument("--key", action="append", default=[], help="NAME=PUBFILE (an ssh-ed25519 public key that may log in as NAME)")
    ap.add_argument("--sftp-root")
    args = ap.parse_args()
    users = dict(u.split(":", 1) for u in args.user)
    keys = {}
    for k in args.key:
        name, path = k.split("=", 1)
        blob = base64.b64decode(open(os.path.expanduser(path)).read().split()[1])
        keys.setdefault(name, []).append(blob[-32:])
    srv = Server(port=args.port, users=users, keys=keys, sftp_root=args.sftp_root)
    print("listening on", srv.port, "host key", srv.fingerprint, flush=True)
    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
