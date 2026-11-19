#!/usr/bin/env python3
"""Reaching a machine through a jump host with the plain ssh client (docs/REMOTE_UPDATE.md). Shared by push-update.py, wol.py and remote-update.py.

A jump host is written [USER@]HOST[:PORT] (HOST: a name, an address, or an IPv6 address in square brackets). The ssh client's ProxyJump makes the connection to the machine *from* the
jump host, and that is all the jump host does: the machine's own ssh host key is checked on this PC, end to end, and the jump host sees only ciphertext.

The options on ssh's command line apply to the machine and not to the jump host, so the jump host's key and options are written to a private ssh config file (ssh -F), which also keeps
quoting out of the way (a ProxyCommand would need it, and the quoting differs on Windows). The options the caller gives come first in that file, because ssh keeps the first value it
finds; then the key; then the defaults; then the user's own ~/.ssh/config, so that what it says about the jump host (its aliases, say) still applies to what is not set here.

    with Hop("me@jump.example", key="~/.ssh/id_ed25519") as hop:
        argv = ["ssh", *hop.ssh_args(), "-o", hop.proxy_jump(), "marisa@192.168.1.185", "true"]       # to the machine, through the jump host
        argv = ["ssh", *hop.ssh_args(), *hop.destination(), "uptime"]                              # a command ON the jump host
"""
import os
import re
import tempfile
from pathlib import Path

_SPEC = re.compile(r"(?:(?P<user>[^@\s]+)@)?(?:\[(?P<v6>[0-9A-Fa-f:.%]+)\]|(?P<host>[^@\s:\[\]]+))(?::(?P<port>[0-9]{1,5}))?")


def parse_via(text):
    """(user or None, host, port or None) from [USER@]HOST[:PORT]; ValueError if it is not one."""
    m = _SPEC.fullmatch((text or "").strip())
    if not m:
        raise ValueError(f"not a jump host: {text!r} (write [USER@]HOST[:PORT]; an IPv6 address goes in square brackets)")
    port = int(m.group("port")) if m.group("port") else None
    if port is not None and not 1 <= port <= 65535:
        raise ValueError(f"not a jump host: {text!r} (port {port})")
    return m.group("user"), m.group("v6") or m.group("host"), port


def _quote(path):
    """A path for a line of an ssh config file: forward slashes (Windows' ssh takes them), in double quotes when it holds a space."""
    p = Path(str(path)).expanduser().as_posix()
    return f'"{p}"' if " " in p else p


class Hop:
    """A jump host and the temporary ssh config file that holds its key and options. Use it as a context manager (or open() and close())."""

    def __init__(self, spec, key=None, options=(), identities_only=True):
        self.spec = spec.strip()
        self.user, self.host, self.port = parse_via(self.spec)
        self.key = key
        self.options = list(options)
        self.identities_only = identities_only
        self.path = None

    # ---- the config file
    def config_text(self):
        lines = [f"Host {self.host}"]
        for o in self.options:                                   # the caller's own words first: ssh keeps the first value
            name, sep, value = o.partition("=")
            if not sep:
                name, _, value = o.partition(" ")
            lines.append(f"    {name.strip()} {value.strip()}")
        if self.key:
            lines.append(f"    IdentityFile {_quote(self.key)}")
            if self.identities_only:
                lines.append("    IdentitiesOnly yes")
        lines += ["    BatchMode yes",                           # never ask anything: a prompt would hang an unattended update
                  "    StrictHostKeyChecking yes",               # the jump host's key must be known already (ssh USER@HOST once); a new one is refused, not trusted
                  "    ConnectTimeout 15", "    ServerAliveInterval 15", "    ServerAliveCountMax 8", "    LogLevel ERROR"]
        user_config = Path.home() / ".ssh" / "config"
        if user_config.is_file():
            lines += ["", f"Include {_quote(user_config)}"]
        return "\n".join(lines) + "\n"

    def open(self):
        if self.path is None:
            fd, name = tempfile.mkstemp(prefix="touhou-hop-", suffix=".conf")
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
                f.write(self.config_text())
            self.path = Path(name)
        return self

    def close(self):
        if self.path is not None:
            try:
                self.path.unlink()
            except OSError:
                pass
            self.path = None

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()

    # ---- what to put on an ssh command line
    def ssh_args(self):
        """-F FILE: the options that make ssh read this jump host's key and options."""
        self.open()
        return ["-F", self.path.as_posix()]

    def proxy_jump(self):
        """The -o option that routes the connection through the jump host."""
        return "ProxyJump=" + self.spec

    def destination(self):
        """The arguments that name the jump host itself as the machine to log in to (to run a command on it)."""
        return (["-p", str(self.port)] if self.port else []) + [f"{self.user}@{self.host}" if self.user else self.host]
