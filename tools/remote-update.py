#!/usr/bin/env python3
"""Updates the machine from far away: wakes it, logs in through a jump host, pushes, confirms (docs/REMOTE_UPDATE.md).

  python tools/remote-update.py setup --via USER@HOST --host ADDRESS --mac MAC --wake-bind IP --broadcast IP [--fingerprint SHA256:...]     write the config, check each hop
  python tools/remote-update.py status                            is the machine on, and what does it run
  python tools/remote-update.py wake                              send the magic packet from the jump host and wait until the machine answers a login
  python tools/remote-update.py push [push-update.py options]     (wakes it if it is off) push build/esp, restart on trial, confirm
  python tools/remote-update.py push --off-after                  ... and switch it off again when it is confirmed (Wake-on-LAN is armed at power-off)
  python tools/remote-update.py off                                sudo poweroff
  python tools/remote-update.py pin                               fetch the machine's ssh host key through the jump host and keep it, if it has the pinned fingerprint

Nothing has to be installed anywhere: the jump host only needs sshd (TCP forwarding for the account) and python3, and the wake script is piped to it over ssh. The machine's own ssh host key is
checked on this PC, end to end (the jump host sees only ciphertext), against the fingerprint in the config. Without --via (on the machine's own network) everything goes directly, and the packet is
sent from this PC.

The config is ~/.touhou-os/remote.json (--config FILE); every value can also be given on the command line, which wins. Keys: via, via_key, via_options, host, user, port, key, ssh_options, mac,
wake_bind, broadcast, wake_seconds, fingerprint, known_hosts, host_key_alias, boot_wait. It holds paths and addresses, no secret. It stays out of the repository.

Exit status: 0 done, 1 something failed (the message says what), 2 bad arguments or config, 5 the jump host cannot be reached or does not take the key, 6 the machine did not come up,
and for `push` the status of push-update.py (3: it came back on the PREVIOUS files, 4: it did not come back in time).
"""
import argparse
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import sshvia  # noqa: E402

CONFIG = Path.home() / ".touhou-os" / "remote.json"
DEFAULTS = {"user": "marisa", "port": 22, "key": "~/.ssh/touhou_ed25519", "via": None, "via_key": None, "via_options": [], "ssh_options": [], "host": None, "mac": None,
            "wake_bind": None, "broadcast": None, "wake_seconds": 20, "fingerprint": None, "known_hosts": "~/.touhou-os/known_hosts", "host_key_alias": "touhou-machine",
            "boot_wait": 300}

# What runs on the jump host to find the machine by its MAC address when its address has changed (a DHCP lease that was given away while it was off): the ARP table, after a few
# datagrams to every address of the /24 (each makes the kernel ask who has it; only the machine that is on answers). Nothing is written anywhere.
DISCOVER_PY = '''import os, socket, sys, time
mac = sys.argv[1].lower().replace("-", ":")
prefix = sys.argv[2]
bind = sys.argv[3] if len(sys.argv) > 3 else ""
arp = os.environ.get("TOUHOU_ARP_FILE", "/proc/net/arp")
def lookup():
    try:
        rows = open(arp).read().splitlines()[1:]
    except OSError:
        return None
    for row in rows:
        f = row.split()
        if len(f) >= 4 and f[3].lower() == mac and f[2] != "0x0":
            return f[0]
    return None
ip = lookup()
if not ip:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if bind:
        s.bind((bind, 0))
    for i in range(1, 255):
        try:
            s.sendto(b"touhou", (prefix + str(i), 9))
        except OSError:
            pass
    time.sleep(3)
    ip = lookup()
print(ip or "")
'''


def load_tool(filename):
    spec = importlib.util.spec_from_file_location(filename.replace("-", "_").replace(".py", ""), TOOLS / filename)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_config(path):
    cfg = dict(DEFAULTS)
    p = Path(path).expanduser()
    if p.is_file():
        try:
            cfg.update({k: v for k, v in json.loads(p.read_text(encoding="utf-8")).items() if k in DEFAULTS})
        except (OSError, ValueError) as e:
            raise SystemExit(f"remote-update: cannot read {p}: {e}")
    return cfg


class PinRefused(Exception):
    """The machine's host key is not the pinned one (or there is nothing to check it against): nothing may be sent to it."""


class Remote:
    """The machine (host, user, key), the jump host in front of it, and what they need: the pinned host key, the wake."""

    POLL = 8                    # seconds between two logins while waiting for the machine
    REWAKE_AFTER = 100          # seconds without an answer before the magic packet is sent once more
    DISCOVER_AFTER = 120        # seconds without an answer before the jump host is asked where the machine has gone

    def __init__(self, cfg, ssh="ssh", say=lambda text: print(text, flush=True)):
        self.cfg = cfg
        self.ssh_exe = ssh
        self.say = say
        self.hop = None
        self.last_err = ""
        self.push_update = load_tool("push-update.py")
        if cfg["via"]:
            self.hop = sshvia.Hop(cfg["via"], key=cfg["via_key"], options=cfg["via_options"])
            self.hop.open()
        self.host = cfg["host"]

    def close(self):
        if self.hop:
            self.hop.close()

    # ---- the pieces of a login
    def known_hosts(self):
        return Path(self.cfg["known_hosts"]).expanduser()

    def machine_options(self):
        """ssh options that are the machine's alone: where its host key is kept and under what name (so that a new address does not matter)."""
        return [f"UserKnownHostsFile={self.known_hosts().as_posix()}", f"HostKeyAlias={self.cfg['host_key_alias']}"] + list(self.cfg["ssh_options"])

    def machine_ssh(self):
        c = self.cfg
        ns = argparse.Namespace(ssh=self.ssh_exe, key=c["key"], port=c["port"], ssh_option=self.machine_options(), host=self.host, user=c["user"], send_timeout=900, hop=self.hop)
        return self.push_update.Ssh(ns)

    def hop_argv(self, *command):
        return [shutil.which(self.ssh_exe) or self.ssh_exe, *self.hop.ssh_args(), *self.hop.destination(), *command]

    def run_on_hop(self, command, stdin=None, timeout=90):
        """(status, stdout, stderr) of a command on the jump host; the status is None on a timeout."""
        feed = {"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}       # (ssh must not read this terminal's input)
        try:
            p = subprocess.run(self.hop_argv(command), capture_output=True, timeout=timeout, **feed)
        except subprocess.TimeoutExpired:
            return None, "", "no answer in time"
        except OSError as e:
            return 1, "", f"cannot run ssh: {e}"
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    # ---- the questions
    def hop_ok(self):
        """(ok, text): can the jump host be logged in to, and has it python3 (which only the wake-up and the search for the machine need: without a MAC address in the config it is not asked for)?"""
        if not self.hop:
            return True, "no jump host: connecting directly"
        wants_python = bool(self.cfg["mac"])
        rc, out, err = self.run_on_hop("python3 -c 'print(1)'" if wants_python else "true", timeout=60)
        if rc == 0 and (out.strip() == "1" or not wants_python):
            return True, f"the jump host {self.cfg['via']} takes the key" + (" and has python3" if wants_python else "")
        return False, f"the jump host {self.cfg['via']} did not answer as needed (status {rc}): {(err or out).strip() or 'no message'}"

    def machine_up(self, timeout=60):
        """True when a login to the machine works (the machine is on, and running a system that lets the key in). Its host key is on file before the first login: a login that found none
        would trust the first key it saw, and that could be anybody's (PinRefused if the key it shows is not the pinned one)."""
        if not self.pinned():
            status, msg = self.pin()
            if status == "refused":
                raise PinRefused(msg)
            if status == "nokey":
                self.last_err = msg
                return False
            self.say("remote-update: " + msg)
        ssh = self.machine_ssh()
        rc, _, _ = ssh.run("true", timeout=timeout)
        self.last_err = ssh.last_err
        return rc == 0

    # ---- the pinned host key
    def pinned(self):
        p = self.known_hosts()
        alias = self.cfg["host_key_alias"]
        try:
            return any(line.split(" ", 1)[0] == alias for line in p.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#"))
        except OSError:
            return False

    def pin(self, trust_first_use=False):
        """Fetches the machine's host key through the same route (authentication is not attempted: the key is all that is wanted), checks its fingerprint against the config and keeps it.
        Returns (status, message): "ok" (kept), "nokey" (the machine gave no key: it is off, or sshd is not running) or "refused" (a key that is not the pinned one, or nothing to check it against)."""
        c = self.cfg
        fd, tmp = tempfile.mkstemp(prefix="touhou-hostkey-")
        os.close(fd)
        try:
            argv = [shutil.which(self.ssh_exe) or self.ssh_exe, "-p", str(c["port"]), "-o", "BatchMode=yes", "-o", f"UserKnownHostsFile={Path(tmp).as_posix()}",
                    "-o", f"HostKeyAlias={c['host_key_alias']}", "-o", "StrictHostKeyChecking=accept-new", "-o", "PreferredAuthentications=none", "-o", "ConnectTimeout=30",
                    "-o", "LogLevel=ERROR"]
            if self.hop:
                argv += self.hop.ssh_args() + ["-o", self.hop.proxy_jump()]
            argv += [f"{c['user']}@{self.host}", "true"]
            try:
                subprocess.run(argv, capture_output=True, timeout=90, stdin=subprocess.DEVNULL)
            except (subprocess.TimeoutExpired, OSError) as e:
                return "nokey", f"could not fetch the host key: {e}"
            lines = [ln for ln in Path(tmp).read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.startswith("#")]
            if not lines:
                return "nokey", f"the machine at {self.host} gave no host key (is it on, and is sshd running?)"
            keygen = shutil.which("ssh-keygen") or "ssh-keygen"
            shown = subprocess.run([keygen, "-lf", tmp], capture_output=True, text=True).stdout
            want = c["fingerprint"]
            good = [ln for ln, fp in zip(lines, shown.splitlines()) if want and want in fp]
            if want and not good:
                return "refused", f"the machine's host key is not the pinned one: it shows {' / '.join(x.split()[1] for x in shown.splitlines() if len(x.split()) > 1)}, the config says {want}. Refusing."
            if not want:
                if not trust_first_use:
                    return "refused", "no fingerprint is configured (--fingerprint SHA256:...), so the host key cannot be checked: it is shown below; give --trust-first-use to keep it as it is\n" + shown
                good = lines
            kh = self.known_hosts()
            kh.parent.mkdir(parents=True, exist_ok=True)
            with open(kh, "a", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(good) + "\n")
            return "ok", f"host key kept in {kh} ({want or 'trusted at first sight'})"
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass

    # ---- wake and wait
    def wake(self):
        """Sends the magic packet: from the jump host when there is one, else from this PC. Returns tools/wol.py's status."""
        c = self.cfg
        if not c["mac"]:
            self.say("remote-update: no MAC address in the config: cannot wake the machine")
            return 2
        argv = [sys.executable, str(TOOLS / "wol.py"), c["mac"], "--seconds", str(c["wake_seconds"])]
        if c["broadcast"]:
            argv += ["--broadcast", c["broadcast"]]
        if c["wake_bind"]:
            argv += ["--bind", c["wake_bind"]]
        if self.hop:
            argv += ["--via", c["via"], "--ssh", self.ssh_exe, "--to", self.host or "192.168.1.185"]
            if c["via_key"]:
                argv += ["--via-key", c["via_key"]]
            for o in c["via_options"]:
                argv += ["--via-option", o]
        elif self.host:
            argv += ["--to", self.host]
        return subprocess.call(argv)

    def discover(self):
        """The machine's address by its MAC, asked on the jump host; None if it is not found (or there is no jump host to ask)."""
        c = self.cfg
        if not (self.hop and c["mac"] and c["broadcast"]):
            return None
        prefix = ".".join(c["broadcast"].split(".")[:3]) + "."
        rc, out, _ = self.run_on_hop(" ".join(shlex.quote(a) for a in ["python3", "-", c["mac"], prefix] + ([c["wake_bind"]] if c["wake_bind"] else [])), stdin=DISCOVER_PY.encode(), timeout=60)
        ip = out.strip().splitlines()[-1] if rc == 0 and out.strip() else ""
        return ip if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", ip) else None

    def wait_up(self, seconds, poll=None):
        """Waits until a login works. Returns True when it does. After two minutes without an answer it asks where the machine has gone (a new address) and goes on with that."""
        poll = self.POLL if poll is None else poll
        end = time.time() + seconds
        t0 = time.time()
        last_say = 0
        rewoken = False
        looked = False
        while True:
            if self.machine_up(timeout=45):
                return True
            now = time.time()
            if now >= end:
                return False
            if now - last_say >= 30:
                self.say(f"remote-update: waiting for the machine ... {int(now - t0)} s")
                last_say = now
            if not rewoken and now - t0 >= self.REWAKE_AFTER and self.cfg["mac"]:
                rewoken = True
                self.say("remote-update: no answer yet: sending the magic packet once more")
                self.wake()
            if not looked and now - t0 >= self.DISCOVER_AFTER:
                looked = True
                ip = self.discover()
                if ip and ip != self.host:
                    self.say(f"remote-update: the machine is at {ip} now (its address was {self.host}); going on with that. `setup --host {ip}` keeps it.")
                    self.host = ip
            time.sleep(poll)

    def ensure_up(self, wake=True):
        """0 when the machine answers a login (waking it first if it does not), 5 when the jump host does not work, 6 when the machine does not come up."""
        ok, text = self.hop_ok()
        self.say("remote-update: " + text)
        if not ok:
            return 5
        if self.machine_up():
            self.say(f"remote-update: the machine is on ({self.host})")
            return 0
        why = " ".join((self.last_err or "no message").split())[:200]
        if not wake:
            self.say(f"remote-update: the machine does not answer ({why})")
            return 6
        self.say(f"remote-update: the machine does not answer ({why}): waking it")
        rc = self.wake()
        if rc != 0:
            self.say(f"remote-update: the wake-up did not go out (status {rc})")
            return 5 if self.hop else 1
        t0 = time.time()
        if self.wait_up(self.cfg["boot_wait"]):
            self.say(f"remote-update: the machine is on after {int(time.time() - t0)} s")
            return 0
        self.say(f"remote-update: the machine did not answer a login within {self.cfg['boot_wait']} s after the wake-up. If its power light is on, its firmware may have started Windows "
                 "(a Windows update can put its own boot entry first), or the new system's network may not be up; the owner has to look. If the light is off, the packet did not reach the "
                 "card: the BIOS needs Wake on LAN set to LAN and Deep Sleep Control off.")
        return 6

    # ---- the commands
    def cmd_status(self):
        rc = self.ensure_up(wake=False)
        if rc == 6:
            self.say("remote-update: status: the machine is off, or running something that does not answer ssh (`wake` turns it on)")
            return 0
        if rc != 0:
            return rc
        ssh = self.machine_ssh()
        code, out, err = ssh.run("sudo update status", timeout=90)
        self.say(out.decode("utf-8", "replace").rstrip() if code == 0 else f"remote-update: `sudo update status` failed ({code}): {err.decode('utf-8', 'replace').strip()}")
        return 0

    def cmd_wake(self):
        return self.ensure_up(wake=True)

    def cmd_off(self):
        rc = self.ensure_up(wake=False)
        if rc != 0:
            return 0 if rc == 6 else rc
        ssh = self.machine_ssh()
        ssh.run("sudo poweroff", timeout=25)                 # (the connection may hang while the machine goes down: it is not waited for)
        end = time.time() + 120
        while time.time() < end:
            time.sleep(4)
            if not self.machine_up(timeout=15):
                self.say("remote-update: the machine is off (it no longer answers); Wake-on-LAN is armed")
                return 0
        self.say("remote-update: the machine still answers two minutes after `poweroff`: look at it")
        return 1

    def push_argv(self, extra):
        c = self.cfg
        argv = ["--host", self.host, "--user", c["user"], "--port", str(c["port"]), "--key", c["key"]]
        for o in self.machine_options():
            argv += ["--ssh-option", o]
        if self.hop:
            argv += ["--via", c["via"]]
            if c["via_key"]:
                argv += ["--via-key", c["via_key"]]
            for o in c["via_options"]:
                argv += ["--via-option", o]
        if self.ssh_exe != "ssh":
            argv += ["--ssh", self.ssh_exe]
        return argv + list(extra)

    def cmd_push(self, extra, off_after=False, no_wake=False):
        # what needs no network first: the files, the initrd, the key guard (so that the machine is not woken for a push that would be refused)
        rc = self.push_update.run(self.push_argv(extra) + ["--dry-run"])
        if rc != 0:
            return rc
        rc = self.ensure_up(wake=not no_wake)
        if rc != 0:
            return rc
        rc = self.push_update.run(self.push_argv(extra))
        if rc == 0 and off_after:
            return self.cmd_off()
        return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["setup", "status", "wake", "push", "off", "pin"])
    ap.add_argument("--config", default=str(CONFIG), help=f"the config file (default {CONFIG})")
    ap.add_argument("--ssh", default="ssh", help="the ssh client (default: ssh from the PATH)")
    for name in ("via", "via-key", "host", "user", "key", "mac", "wake-bind", "broadcast", "fingerprint", "known-hosts", "host-key-alias"):
        ap.add_argument("--" + name, default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--via-option", dest="via_options", action="append", default=None, help="an ssh option for the jump host only (repeatable)")
    ap.add_argument("--ssh-option", dest="ssh_options", action="append", default=None, help="an ssh option for the machine only (repeatable)")
    ap.add_argument("--wake-seconds", type=int, default=None)
    ap.add_argument("--boot-wait", type=int, default=None, help="seconds to wait for the machine to answer after the wake-up (default 300)")
    ap.add_argument("--no-wake", action="store_true", help="(push) do not wake the machine: stop if it is off")
    ap.add_argument("--off-after", action="store_true", help="(push) switch the machine off after a confirmed update")
    ap.add_argument("--trust-first-use", action="store_true", help="(pin) keep the host key without a fingerprint to check it against: for a machine that is not the real one only")
    args, rest = ap.parse_known_args(argv)
    if rest and rest[0] == "--":
        rest = rest[1:]
    if rest and args.command != "push":
        ap.error("unexpected arguments: " + " ".join(rest))
    cfg = load_config(args.config)
    for key in ("via", "via_key", "host", "user", "key", "mac", "wake_bind", "broadcast", "fingerprint", "known_hosts", "host_key_alias", "port", "via_options", "ssh_options",
                "wake_seconds", "boot_wait"):
        v = getattr(args, key)
        if v is not None:
            cfg[key] = v
    if args.command == "setup":
        return setup(args, cfg)
    if not cfg["host"]:
        print("remote-update: no machine address: give --host ADDRESS or run `setup` (the config is " + str(args.config) + ")", file=sys.stderr)
        return 2
    try:
        r = Remote(cfg, ssh=args.ssh)
    except ValueError as e:
        print(f"remote-update: {e}", file=sys.stderr)
        return 2
    try:
        if args.command == "status":
            return r.cmd_status()
        if args.command == "wake":
            return r.cmd_wake()
        if args.command == "off":
            return r.cmd_off()
        if args.command == "pin":
            ok, text = r.hop_ok()
            print("remote-update: " + text)
            if not ok:
                return 5
            status, text = r.pin(args.trust_first_use)
            print("remote-update: " + text)
            return 0 if status == "ok" else 1
        return r.cmd_push(rest, off_after=args.off_after, no_wake=args.no_wake)
    except PinRefused as e:
        print("remote-update: " + str(e), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nremote-update: interrupted. An update that had begun on the machine finishes by itself (see push-update.py); `status` shows where it stands.", file=sys.stderr)
        return 130
    finally:
        r.close()


def setup(args, cfg):
    """Writes the config (what was given on the command line over what was there) and checks what can be checked."""
    path = Path(args.config).expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    keep = {k: v for k, v in cfg.items() if v != DEFAULTS.get(k)}
    path.write_text(json.dumps(keep, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"remote-update: config written to {path}:")
    for k, v in keep.items():
        print(f"  {k}: {v}")
    if not cfg["host"]:
        print("remote-update: still missing: --host (the machine's address as the jump host sees it)")
        return 2
    try:
        r = Remote(cfg, ssh=args.ssh)
    except ValueError as e:
        print(f"remote-update: {e}", file=sys.stderr)
        return 2
    try:
        ok, text = r.hop_ok()
        print("remote-update: " + text)
        if not ok:
            return 5
        if r.hop and cfg["wake_bind"]:
            rc, out, _ = r.run_on_hop("python3 -c " + shlex.quote("import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect((%r, 9)); print(s.getsockname()[0])" % cfg["host"]), timeout=60)
            mine = out.strip()
            if rc == 0 and mine:
                print(f"remote-update: from the jump host the machine's address {cfg['host']} is reached from {mine}" + ("" if mine == cfg["wake_bind"] else f" (the config sends the magic packet from {cfg['wake_bind']}: they should be the same network)"))
        if not cfg["mac"]:
            print("remote-update: no --mac: the machine cannot be woken, only updated while it is on")
        up = r.machine_up()
        print(f"remote-update: the machine {'answers a login' if up else 'does not answer (off? then `wake` and `status` check it)'}")
        if up and not r.pinned():
            status, text = r.pin(args.trust_first_use)
            print("remote-update: " + text)
            return 0 if status == "ok" else 1
        if not r.pinned():
            print("remote-update: the machine's host key is fetched and checked the first time it is on (`pin`, or the next `wake`/`push`/`status`)")
        return 0
    finally:
        r.close()


if __name__ == "__main__":
    sys.exit(main())
