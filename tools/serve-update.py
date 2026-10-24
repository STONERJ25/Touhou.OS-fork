#!/usr/bin/env python3
"""serve-update -- Yukari's delivery gap: a signed update manifest and the files it names, over plain HTTP on the LAN.

  python tools/serve-update.py keygen [--out PATH] [--encrypt]       make the signing key (outside the repository)
  python tools/serve-update.py pubkey [--format hex|pem|c] [--out F]  the public half, to bake into the OS image
  python tools/serve-update.py manifest [--out FILE]                 build and sign a manifest of build/esp, print or write it
  python tools/serve-update.py serve [--bind 0.0.0.0] [--port 8080]  build, sign and serve manifest and files
  python tools/serve-update.py verify MANIFEST [--pub KEY] [--src DIR]   check a manifest (and the files in DIR)
  python tools/serve-update.py check URL [--pub KEY]                 fetch from a running server and check everything

An installed TouhouOS boots from an EFI System Partition that holds EFI/BOOT/BOOTX64.EFI, kernel.elf, initrd.img and
cmdline.txt (the layout of build/esp and of tools/mkimage.py). This tool lists those four files with their sizes and
SHA-256 hashes in a small text manifest, signs it with ECDSA P-256, and serves the manifest at /manifest and each file at
/files/<path>. The OS side (the `update URL` tool, not written yet) checks the signature with the public key baked into
its image, then the hash of every file it downloads. docs/UPDATE.md has the exact format and the update protocol.

Where the files come from: --src (default build/esp, what `python tools/build.py` produces). cmdline.txt is special:
build/esp/cmdline.txt is rewritten by every test run, so with the default source the manifest carries tools/mkimage.py's
default command line instead, unless --cmdline gives one; with another --src its cmdline.txt is used as it is;
--no-cmdline leaves the file out so the installed system keeps its own.

The signing key: `keygen` writes a PEM file (PKCS#8, mode 0600) to --out, default $TOUHOU_UPDATE_KEY or
~/.touhou-os/update-signing-key.pem. It refuses a path inside this repository unless git ignores that path. Every other
command finds the key the same way (--key, then $TOUHOU_UPDATE_KEY, then the default). An encrypted key asks for its
passphrase, or takes it from $TOUHOU_UPDATE_KEY_PASSPHRASE.

Needs the Python package `cryptography`. Runs on Linux and Windows.
"""
import argparse
import getpass
import hashlib
import http.server
import os
import re
import socket
import subprocess
import socketserver
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import Prehashed, decode_dss_signature, encode_dss_signature
except ImportError:                                     # the error is reported when a command needs it
    ec = None

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SRC = REPO / "build" / "esp"
DEFAULT_CMDLINE = "video=native blk.ro"                 # the same default as tools/mkimage.py
DEFAULT_PORT = 8080
KEY_ENV = "TOUHOU_UPDATE_KEY"
PASS_ENV = "TOUHOU_UPDATE_KEY_PASSPHRASE"

# The files an installed system boots from, by their path on the ESP. Kept in the manifest's order (ascending bytes).
ESP_FILES = ("EFI/BOOT/BOOTX64.EFI", "cmdline.txt", "initrd.img", "kernel.elf")

# ---------------------------------------------------------------------------------------------------- the format

MAGIC = b"touhou-update 1"
MAX_MANIFEST = 4096                                     # bytes, signature line included
MAX_FILES = 16
MAX_SIZE = 10 ** 12 - 1                                 # at most 12 decimal digits
SIG_PREFIX = b"signature "

RE_VERSION = re.compile(rb"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")
RE_NUMBER = re.compile(rb"0|[1-9][0-9]{0,11}")
RE_ISSUED = re.compile(rb"0|[1-9][0-9]{0,15}")
RE_KEYID = re.compile(rb"[0-9a-f]{16}")
RE_SHA = re.compile(rb"[0-9a-f]{64}")
RE_SIG = re.compile(rb"[0-9a-f]{128}")
RE_COMPONENT = re.compile(rb"[A-Za-z0-9_][A-Za-z0-9._-]{0,63}")


class ManifestError(ValueError):
    """The manifest is malformed, signed by another key, or its signature is wrong."""


@dataclass(frozen=True)
class Entry:
    path: str
    size: int
    sha256: str                                         # 64 lowercase hex digits


@dataclass(frozen=True)
class Manifest:
    version: str
    issued: int
    key_id: str
    files: tuple                                        # of Entry, in ascending path order
    body: bytes                                         # the signed bytes: from the first byte up to and including "end\n"
    signature: bytes                                    # 64 bytes: r || s, each 32 bytes big-endian

    def entry(self, path):
        for e in self.files:
            if e.path == path:
                return e
        return None


def valid_path(p):
    """A relative path of 1-8 components, each [A-Za-z0-9_][A-Za-z0-9._-]*, so '.', '..', '' and backslashes cannot occur."""
    b = p.encode("ascii", "replace") if isinstance(p, str) else p
    if not 1 <= len(b) <= 128:
        return False
    parts = b.split(b"/")
    return len(parts) <= 8 and all(RE_COMPONENT.fullmatch(c) for c in parts)


def sanitize_version(text):
    """Turns any string (git describe output, a user's --version) into a token the format accepts."""
    t = re.sub(r"[^A-Za-z0-9._+-]", "-", text.strip())
    t = t.lstrip("._+-")[:64]
    return t or "unknown"


def encode_body(version, issued, key_id, entries):
    """The canonical manifest body (the bytes that are signed). Raises ManifestError for anything the format forbids."""
    lines = [MAGIC, b"version " + version.encode("ascii"), b"issued %d" % issued, b"key " + key_id.encode("ascii")]
    ents = sorted(entries, key=lambda e: e.path.encode("ascii"))
    for e in ents:
        lines.append(b"file %s %d %s" % (e.path.encode("ascii"), e.size, e.sha256.encode("ascii")))
    lines.append(b"end")
    body = b"\n".join(lines) + b"\n"
    parse_body(body)                                    # one grammar for both directions
    return body


def parse_body(body):
    """Strict parse of a manifest body; returns (version, issued, key_id, files)."""
    if not body.endswith(b"\n"):
        raise ManifestError("the body does not end with a newline")
    if any(c > 0x7E or (c < 0x20 and c != 0x0A) for c in body):
        raise ManifestError("only printable ASCII and LF may appear")
    lines = body[:-1].split(b"\n")
    if len(lines) < 6:
        raise ManifestError("too few lines")
    if lines[0] != MAGIC:
        raise ManifestError("not a touhou-update 1 manifest")

    def field(line, name, rx):
        head = name + b" "
        if not line.startswith(head) or not rx.fullmatch(line[len(head):]):
            raise ManifestError(f"expected a valid '{name.decode()}' line, got {line[:80]!r}")
        return line[len(head):].decode("ascii")

    version = field(lines[1], b"version", RE_VERSION)
    issued = int(field(lines[2], b"issued", RE_ISSUED))
    key_id = field(lines[3], b"key", RE_KEYID)
    if lines[-1] != b"end":
        raise ManifestError("the last body line is not 'end'")
    files = []
    for line in lines[4:-1]:
        parts = line.split(b" ")
        if len(parts) != 4 or parts[0] != b"file":
            raise ManifestError(f"expected 'file PATH SIZE SHA256', got {line[:80]!r}")
        _, path, size, sha = parts
        if not valid_path(path):
            raise ManifestError(f"refused path {path[:80]!r}")
        if not RE_NUMBER.fullmatch(size) or int(size) > MAX_SIZE:
            raise ManifestError(f"bad size {size[:20]!r} for {path.decode()}")
        if not RE_SHA.fullmatch(sha):
            raise ManifestError(f"bad sha256 for {path.decode()}")
        if files and path <= files[-1].path.encode("ascii"):
            raise ManifestError("file lines must be in strictly ascending byte order of their paths")
        files.append(Entry(path.decode("ascii"), int(size), sha.decode("ascii")))
    if not 1 <= len(files) <= MAX_FILES:
        raise ManifestError(f"a manifest names 1 to {MAX_FILES} files")
    return version, issued, key_id, tuple(files)


def split_manifest(data):
    """(body, signature bytes) of a whole manifest; the signature line must be the last line and nothing may follow it."""
    if len(data) > MAX_MANIFEST:
        raise ManifestError(f"larger than {MAX_MANIFEST} bytes")
    if not data.endswith(b"\n"):
        raise ManifestError("the manifest does not end with a newline")
    cut = data.rfind(b"\n", 0, len(data) - 1) + 1         # start of the last line
    last = data[cut:-1]
    if cut == 0 or not last.startswith(SIG_PREFIX) or not RE_SIG.fullmatch(last[len(SIG_PREFIX):]):
        raise ManifestError("the last line is not 'signature <128 hex digits>'")
    return data[:cut], bytes.fromhex(last[len(SIG_PREFIX):].decode("ascii"))


def parse_manifest(data):
    """Parses without checking the signature (verify_manifest does both)."""
    body, sig = split_manifest(data)
    version, issued, key_id, files = parse_body(body)
    return Manifest(version, issued, key_id, files, body, sig)


# ---------------------------------------------------------------------------------------------------- keys and signatures

def need_crypto():
    if ec is None:
        sys.exit("serve-update: the Python package 'cryptography' is missing (pip install cryptography)")


def public_point(key):
    """The 65-byte uncompressed point 04 || X || Y of a P-256 public or private key."""
    pub = key.public_key() if hasattr(key, "public_key") else key
    return pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def key_id_of(key):
    """The first 8 bytes of SHA-256 over the uncompressed point, as 16 hex digits."""
    return hashlib.sha256(public_point(key)).hexdigest()[:16]


def check_curve(key, what):
    if not isinstance(key, (ec.EllipticCurvePrivateKey, ec.EllipticCurvePublicKey)) or key.curve.name != "secp256r1":
        raise ManifestError(f"{what} is not an ECDSA P-256 key")
    return key


def sign_body(body, private_key):
    """The signature line for a body: ECDSA P-256 over SHA-256(body), r || s as 128 lowercase hex digits."""
    der = private_key.sign(body, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return SIG_PREFIX + (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex().encode("ascii") + b"\n"


def verify_manifest(data, public_key):
    """Parses strictly, checks the key id and the signature; returns the Manifest or raises ManifestError.
    The check is the one the OS makes: SHA-256 of the body, then ECDSA verification with r and s taken from the signature."""
    m = parse_manifest(data)
    if m.key_id != key_id_of(public_key):
        raise ManifestError(f"signed by key {m.key_id}, not by {key_id_of(public_key)}")
    r, s = int.from_bytes(m.signature[:32], "big"), int.from_bytes(m.signature[32:], "big")
    digest = hashlib.sha256(m.body).digest()
    pub = public_key.public_key() if hasattr(public_key, "public_key") else public_key
    try:
        pub.verify(encode_dss_signature(r, s), digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    except (InvalidSignature, ValueError):
        raise ManifestError("the signature does not match (the manifest was changed, or signed by another key)") from None
    return m


def check_file(entry, data):
    """Raises ManifestError unless data has the size and SHA-256 the manifest promises."""
    if len(data) != entry.size:
        raise ManifestError(f"{entry.path}: {len(data)} bytes, the manifest says {entry.size}")
    if hashlib.sha256(data).hexdigest() != entry.sha256:
        raise ManifestError(f"{entry.path}: the sha256 does not match the manifest")


def build_manifest(files, private_key, version, issued=None):
    """files: {esp path: bytes}. Returns the signed manifest bytes."""
    entries = [Entry(p, len(d), hashlib.sha256(d).hexdigest()) for p, d in files.items()]
    for e in entries:
        if not valid_path(e.path):
            raise ManifestError(f"refused path {e.path!r}")
    issued = int(time.time()) if issued is None else int(issued)
    body = encode_body(sanitize_version(version), issued, key_id_of(private_key), entries)
    return body + sign_body(body, private_key)


def default_key_path():
    env = os.environ.get(KEY_ENV)
    return Path(env).expanduser() if env else Path.home() / ".touhou-os" / "update-signing-key.pem"


def key_location_problem(path):
    """None if a private key may be written to path; otherwise why not (inside the repository and not ignored by git)."""
    p = Path(path).expanduser().resolve()
    try:
        p.relative_to(REPO.resolve())
    except ValueError:
        return None
    try:
        r = subprocess.run(["git", "-C", str(REPO), "check-ignore", "-q", "--no-index", str(p)], capture_output=True)
    except OSError:
        return f"{p} is inside the repository and git is not available to check that it is ignored"
    if r.returncode == 0:
        return None
    return f"{p} is inside the repository and git does not ignore it: the private key must never be committed"


def load_private_key(path):
    need_crypto()
    data = Path(path).expanduser().read_bytes()
    try:
        key = serialization.load_pem_private_key(data, password=None)
    except TypeError:                                   # encrypted: the passphrase comes from the environment or the terminal
        pw = os.environ.get(PASS_ENV)
        pw = pw.encode() if pw is not None else getpass.getpass(f"passphrase for {path}: ").encode()
        key = serialization.load_pem_private_key(data, password=pw)
    return check_curve(key, str(path))


def load_public_key(path):
    """A PEM public key, a PEM private key (its public half), or a file holding the 130 hex digits of the uncompressed point."""
    need_crypto()
    data = Path(path).expanduser().read_bytes()
    text = data.strip()
    if re.fullmatch(rb"04[0-9a-fA-F]{128}", text):
        return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(text.decode()))
    if b"PRIVATE KEY" in data:
        return load_private_key(path).public_key()
    return check_curve(serialization.load_pem_public_key(data), str(path))


def write_private_key(path, key, passphrase=None):
    """Writes a new PEM file with mode 0600 (the directory 0700 when it is created); refuses to replace an existing file."""
    p = Path(path).expanduser()
    p.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    enc = serialization.BestAvailableEncryption(passphrase) if passphrase else serialization.NoEncryption()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, enc)
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    return p


def c_header(key):
    pt = public_point(key)
    rows = ",\n".join("    " + ", ".join(f"0x{b:02x}" for b in pt[i:i + 13]) for i in range(0, len(pt), 13))
    return (f"/* TouhouOS update signing key: ECDSA P-256, uncompressed point 04 || X || Y, key id {key_id_of(key)}.\n"
            f" * Made by tools/serve-update.py pubkey --format c; the private half stays outside the repository. See docs/UPDATE.md. */\n"
            f"#define TOUHOU_UPDATE_KEY_ID \"{key_id_of(key)}\"\n"
            f"static const unsigned char touhou_update_pubkey[65] = {{\n{rows}\n}};\n")


# ---------------------------------------------------------------------------------------------------- collecting the files

def git_version():
    try:
        r = subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty", "--tags"], capture_output=True, text=True)
    except OSError:
        return "unknown"
    return sanitize_version(r.stdout) if r.returncode == 0 else "unknown"


def collect_files(src=None, cmdline=None, with_cmdline=True):
    """{esp path: bytes} of the files an installed system needs, read once (the server keeps exactly these bytes).
    Returns (files, note) where note says where cmdline.txt came from."""
    src = Path(src) if src is not None else DEFAULT_SRC
    files, missing = {}, []
    for rel in ESP_FILES:
        if rel == "cmdline.txt":
            continue
        f = src.joinpath(*rel.split("/"))
        if f.is_file():
            files[rel] = f.read_bytes()
        else:
            missing.append(str(f))
    if missing:
        raise FileNotFoundError("missing (run python tools/build.py, or pass --src): " + ", ".join(missing))
    note = "left out (--no-cmdline): the installed cmdline.txt stays"
    if with_cmdline:
        if cmdline is not None:
            text, note = cmdline, "from --cmdline"
        elif src.resolve() == DEFAULT_SRC.resolve() or not (src / "cmdline.txt").is_file():
            text, note = DEFAULT_CMDLINE, "the default of tools/mkimage.py (build/esp/cmdline.txt belongs to the tests)"
        else:
            text, note = None, f"from {src / 'cmdline.txt'}"
            files["cmdline.txt"] = (src / "cmdline.txt").read_bytes()
        if text is not None:
            try:
                files["cmdline.txt"] = (text.strip() + "\n").encode("ascii")
            except UnicodeEncodeError:
                raise ValueError("the kernel command line must be plain ASCII") from None
    return dict(sorted(files.items())), note


# ---------------------------------------------------------------------------------------------------- the HTTP server

class UpdateHandler(http.server.BaseHTTPRequestHandler):
    """GET/HEAD /manifest, /files/<path> for the paths the manifest names (nothing else, compared as raw bytes: no
    percent-decoding, no normalisation, so '..' and friends can never reach the disk) and / for a short index."""
    server_version = "TouhouUpdate/1"
    protocol_version = "HTTP/1.0"                       # one request per connection: the simplest thing for the OS's client

    def _route(self):
        path = self.path.split("?", 1)[0]
        st = self.server.state
        if path == "/manifest":
            return st["manifest"], "text/plain; charset=us-ascii"
        if path.startswith("/files/") and path[len("/files/"):] in st["files"]:
            return st["files"][path[len("/files/"):]], "application/octet-stream"
        if path == "/":
            lines = [f"TouhouOS update gap: version {st['version']}", "", "/manifest"]
            lines += [f"/files/{p}  ({len(d)} bytes)" for p, d in st["files"].items()]
            return ("\n".join(lines) + "\n").encode(), "text/plain; charset=us-ascii"
        return None, None

    def _answer(self, with_body):
        data, ctype = self._route()
        if data is None:
            self.send_response(404)
            body = b"404: no such thing behind this gap\n"
            self.send_header("Content-Type", "text/plain; charset=us-ascii")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if with_body:
                self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if with_body:
            self.wfile.write(data)

    def do_GET(self):
        self._answer(True)

    def do_HEAD(self):
        self._answer(False)

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            sys.stderr.write(f"serve-update: {self.client_address[0]} {fmt % args}\n")


class UpdateServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, bind, port, manifest, files, quiet=False):
        if ":" in bind:
            self.address_family = socket.AF_INET6
        self.state = {"manifest": manifest, "files": dict(files), "version": parse_manifest(manifest).version}
        self.quiet = quiet
        super().__init__((bind, port), UpdateHandler)

    def server_bind(self):                              # HTTPServer's version asks DNS for the host name, which can stall
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def make_server(bind, port, manifest, files, quiet=False):
    """A server that serves exactly the manifest and the bytes given (files: {esp path: bytes}); port 0 picks a free one.
    Call serve_forever() (in a thread if you like) and shutdown() / server_close() when done."""
    m = parse_manifest(manifest)
    names = {e.path for e in m.files}
    if names != set(files):
        raise ValueError("the files to serve are not the ones the manifest names")
    for e in m.files:
        check_file(e, files[e.path])
    return UpdateServer(bind, port, manifest, files, quiet)


def lan_addresses():
    """Best guess at the addresses other machines reach this one by (no packet is sent)."""
    out = []
    for probe in ("10.255.255.255", "192.168.255.255"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((probe, 1))
                a = s.getsockname()[0]
            if a not in out and not a.startswith("127."):
                out.append(a)
        except OSError:
            pass
    return out


# ---------------------------------------------------------------------------------------------------- fetching (the reference client)

def fetch(url, limit):
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise ManifestError(f"{url}: more than {limit} bytes")
    return data


def check_server(base, public_key):
    """What the OS will do, minus the writing: fetch /manifest, verify it, fetch every file, check size and hash."""
    base = base if base.endswith("/") else base + "/"
    m = verify_manifest(fetch(urllib.parse.urljoin(base, "manifest"), MAX_MANIFEST), public_key)
    for e in m.files:
        check_file(e, fetch(urllib.parse.urljoin(base, "files/" + e.path), e.size))
    return m


# ---------------------------------------------------------------------------------------------------- commands

def describe(m):
    when = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(m.issued))
    lines = [f"  version {m.version}, issued {m.issued} ({when}), key {m.key_id}"]
    lines += [f"  {e.sha256[:16]}  {e.size:>10}  {e.path}" for e in m.files]
    return "\n".join(lines)


def cmd_keygen(a):
    need_crypto()
    path = Path(a.out).expanduser() if a.out else default_key_path()
    problem = key_location_problem(path)
    if problem:
        sys.exit(f"serve-update: refused: {problem}")
    if path.exists():
        sys.exit(f"serve-update: {path} exists; images that carry its public key would stop accepting updates "
                 f"if it were replaced. Delete it yourself if that is what you want.")
    pw = None
    if a.encrypt:
        pw = getpass.getpass("new passphrase: ").encode()
        if pw != getpass.getpass("again: ").encode():
            sys.exit("serve-update: the passphrases differ")
    key = ec.generate_private_key(ec.SECP256R1())
    write_private_key(path, key, pw)
    print(f"serve-update: the shrine's seal is made: {path} (private, ECDSA P-256, key id {key_id_of(key)})")
    print(f"  public key: {public_point(key).hex()}")
    print("  keep the private key off the repository and back it up; bake the public key into the image with")
    print("  python tools/serve-update.py pubkey --format c")


def cmd_pubkey(a):
    key = load_public_key(a.key or default_key_path())
    if a.format == "hex":
        text = public_point(key).hex() + "\n"
    elif a.format == "pem":
        text = key.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    else:
        text = c_header(key)
    if a.out:
        Path(a.out).write_text(text, newline="\n")
        print(f"serve-update: wrote {a.out} (key id {key_id_of(key)})")
    else:
        sys.stdout.write(text)


def _signed(a):
    files, note = collect_files(a.src, a.cmdline, not a.no_cmdline)
    key = load_private_key(a.key or default_key_path())
    manifest = build_manifest(files, key, a.version or git_version(), a.issued)
    return manifest, files, note


def cmd_manifest(a):
    manifest, _, note = _signed(a)
    if a.out:
        Path(a.out).write_bytes(manifest)
        m = parse_manifest(manifest)
        print(f"serve-update: wrote {a.out}\n{describe(m)}\n  cmdline.txt: {note}")
    else:
        sys.stdout.buffer.write(manifest)


def cmd_serve(a):
    manifest, files, note = _signed(a)
    m = parse_manifest(manifest)
    srv = make_server(a.bind, a.port, manifest, files)
    host, port = srv.server_address[:2]
    print(f"serve-update: Yukari opened a gap at http://{host}:{port}/ (manifest at /manifest, files at /files/<path>)")
    print(describe(m))
    print(f"  cmdline.txt: {note}")
    if host in ("0.0.0.0", "::"):
        for addr in lan_addresses():
            print(f"  from the LAN: update http://{addr}:{port}/")
    print("  the files are read once: rebuild and restart this server to publish a new version. Ctrl-C closes the gap.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nserve-update: the gap closes")
    finally:
        srv.server_close()


def cmd_verify(a):
    key = load_public_key(a.pub or a.key or default_key_path())
    try:
        m = verify_manifest(Path(a.manifest).read_bytes(), key)
        if a.src:
            for e in m.files:
                f = Path(a.src).joinpath(*e.path.split("/"))
                if not f.is_file():
                    raise ManifestError(f"{e.path}: not in {a.src}")
                check_file(e, f.read_bytes())
    except ManifestError as ex:
        sys.exit(f"serve-update: REFUSED: {ex}")
    print(f"serve-update: the seal holds{' and every file matches' if a.src else ''}\n{describe(m)}")


def cmd_check(a):
    key = load_public_key(a.pub or a.key or default_key_path())
    try:
        m = check_server(a.url, key)
    except (ManifestError, urllib.error.URLError, OSError) as ex:
        sys.exit(f"serve-update: REFUSED: {ex}")
    print(f"serve-update: {a.url}: the seal holds and every file matches\n{describe(m)}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="serve-update.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def key_arg(p):
        p.add_argument("--key", help=f"the private key (default ${KEY_ENV} or ~/.touhou-os/update-signing-key.pem)")

    p = sub.add_parser("keygen", help="make a new ECDSA P-256 signing key")
    p.add_argument("--out", help=f"where to write it (default ${KEY_ENV} or ~/.touhou-os/update-signing-key.pem)")
    p.add_argument("--encrypt", action="store_true", help="protect the file with a passphrase")
    p.set_defaults(fn=cmd_keygen)

    p = sub.add_parser("pubkey", help="print or write the public key")
    key_arg(p)
    p.add_argument("--format", choices=("hex", "pem", "c"), default="hex", help="hex: 130 digits of 04||X||Y; c: a header")
    p.add_argument("--out", help="write to this file instead of standard output")
    p.set_defaults(fn=cmd_pubkey)

    for name, fn, what in (("manifest", cmd_manifest, "build and sign a manifest"), ("serve", cmd_serve, "build, sign and serve")):
        p = sub.add_parser(name, help=what)
        key_arg(p)
        p.add_argument("--src", help="directory laid out like the ESP (default build/esp)")
        p.add_argument("--cmdline", help="the kernel command line to ship as cmdline.txt")
        p.add_argument("--no-cmdline", action="store_true", help="leave cmdline.txt out of the update")
        p.add_argument("--version", help="version string (default: git describe --always --dirty --tags)")
        p.add_argument("--issued", type=int, help="issue time in Unix seconds (default: now)")
        if name == "manifest":
            p.add_argument("--out", help="write the manifest here instead of standard output")
        else:
            p.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0: every interface)")
            p.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"port (default {DEFAULT_PORT}; 0 picks a free one)")
        p.set_defaults(fn=fn)

    p = sub.add_parser("verify", help="check a manifest file and optionally the files in a directory")
    p.add_argument("manifest")
    key_arg(p)
    p.add_argument("--pub", help="public key (PEM, or 130 hex digits); a private key works too")
    p.add_argument("--src", help="also check the files in this directory")
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("check", help="fetch manifest and files from a server and check them all")
    p.add_argument("url", help="the server's base URL, e.g. http://192.168.1.10:8080/")
    key_arg(p)
    p.add_argument("--pub", help="public key (PEM, or 130 hex digits); a private key works too")
    p.set_defaults(fn=cmd_check)

    a = ap.parse_args(argv)
    try:
        a.fn(a)
    except (OSError, ManifestError, ValueError) as ex:
        sys.exit(f"serve-update: {ex}")


if __name__ == "__main__":
    main()
