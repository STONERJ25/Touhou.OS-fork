#!/usr/bin/env python3
"""Watches a TouhouOS machine restart (or wake up), and can confirm a trial update when it is back.

  python tools/machine-watch.py restart "sudo reboot"        run the command on the machine, then wait until it has really restarted
  python tools/machine-watch.py wait                         wait for a restart that something else causes (a push, a button, a magic packet)
  python tools/machine-watch.py confirm VERSION [--dry]      wait for a restart, then `sudo update confirm` -- only if the machine reports a start on trial of VERSION

A restart is proved by a token this script leaves in the machine's /tmp over a login (RAM, empty at every start): the machine has started since when a later login finds the token gone.
It is never proved by what answers on port 22: the machine's sshd turns an address away for a minute after five connections that ended without a login, so a script that polled the
port made the old start look like a restart and confirmed it (2026-10-25, docs/UPDATE.md); nor by the uptime, which lags on a busy virtual machine. Ping (ICMP) only paces the logins.

Options: --host (default 192.168.1.185), --user marisa, --key ~/.ssh/touhou_ed25519, --known-hosts assets/local/dell_known_hosts, --wait SECONDS.
Exit status: 0 restarted (confirm: confirmed, or with --dry would have been), 1 the confirmation failed / still the same start, 2 restarted but not a trial start of VERSION, 3 no baseline (the machine did not
answer a login at the start), 4 gave up waiting, 5 the host key changed (nothing is trusted), 6 restarted on the PREVIOUS files (the new ones did not come up).
"""
import argparse
import os
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOKEN_PATH = "/tmp/.machine-watch-token"


def now():
    return time.strftime("%H:%M:%S")


def log(*a):
    print(now(), *a, flush=True)


class Machine:
    def __init__(self, args):
        self.host = args.host
        known = Path(args.known_hosts)
        self.base = ["ssh", "-i", str(Path(args.key).expanduser()), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                     "-o", f"UserKnownHostsFile={known.as_posix()}", "-o", "ConnectTimeout=10", "-q", f"{args.user}@{args.host}"]

    def ping(self):
        cmd = ["ping", "-n", "1", "-w", "1000", self.host] if os.name == "nt" else ["ping", "-c", "1", "-W", "1", self.host]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            return False
        return "TTL=" in p.stdout.upper() or "ttl=" in p.stdout        # only a real echo reply has it ("Destination host unreachable" exits with 0 too)

    def ssh(self, command, timeout=60):
        try:
            p = subprocess.run(self.base + [command], capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 255, "", "timeout"
        return p.returncode, p.stdout, p.stderr


def classify(rc, out, token):
    """What one login says about the token: "same" (it is there: the start it was planted in), "new" (the login works and it is gone: a new start), None (no answer)."""
    if rc != 0 or "END" not in out:
        return None
    return "same" if token in out else "new"


def look(m, token):
    rc, out, err = m.ssh(f"cat {TOKEN_PATH} 2>&1; echo END", timeout=30)
    if rc != 0 and ("REMOTE HOST IDENTIFICATION" in err or "Host key verification failed" in err):
        log("HOST KEY CHANGED: nothing is trusted; stopping.")
        sys.exit(5)
    return classify(rc, out, token)


def plant(m):
    token = secrets.token_hex(8)
    rc, out, err = m.ssh(f"echo {token} > {TOKEN_PATH}; cat {TOKEN_PATH}; echo END")
    if rc != 0 or token not in out:
        if "REMOTE HOST IDENTIFICATION" in err or "Host key verification failed" in err:
            log("HOST KEY CHANGED: nothing is trusted; stopping.")
            sys.exit(5)
        log("no baseline:", rc, out.strip()[:200], err.strip()[:200])
        sys.exit(3)
    return token


def wait_restart(m, token, deadline, t_cmd=None, give_up_same=None):
    """"restarted", "same" (still the old start long after the command) or "timeout"."""
    down, was_down, up_since, last_login = 0, False, None, 0.0
    while time.time() < deadline:
        if not m.ping():
            down += 1
            up_since = None
            if down == 3 and not was_down:
                was_down = True
                log("the machine stopped answering ping")
            time.sleep(2)
            continue
        down = 0
        if up_since is None:
            up_since = time.time()
            if was_down:
                log("the machine answers ping again")
        wait = 8 if was_down else (15 if t_cmd else 90)               # (a restart that ping missed is still found, slowly)
        if time.time() - last_login < wait or (was_down and time.time() - up_since < 6):
            time.sleep(2)
            continue
        last_login = time.time()
        seen = look(m, token)
        if seen is None:
            log("ping answers, ssh not yet")
            continue
        if seen == "new":
            log("RESTARTED: the token is gone")
            return "restarted"
        if was_down:
            log("the token is still there: no restart (a network gap only)")
            was_down = False
        if give_up_same and t_cmd and time.time() - t_cmd > give_up_same:
            log(f"still the same start {give_up_same} s after the command: the restart did nothing")
            return "same"
    return "timeout"


def confirm_trial(m, version, dry):
    rc, out, err = m.ssh("sudo update status")
    if rc != 0:
        log("status failed:", rc, err.strip()[:300])
        return 1
    print(out, flush=True)
    if "this start: the PREVIOUS kernel and initrd" in out:
        log("it came back on the PREVIOUS files: the new ones did not come up in time. Nothing confirmed.")
        return 6
    found = re.search(r"installed by the last push: version (\S+?),", out)
    if "this start: the new files, on trial" in out and "update.try: present" in out and found and found.group(1) == version:
        if dry:
            log("dry run: would confirm now")
            return 0
        rc, out2, err2 = m.ssh("sudo update confirm")
        print(out2, err2, flush=True)
        log("CONFIRMED" if rc == 0 else f"confirm failed ({rc})")
        return 0 if rc == 0 else 1
    log(f"restarted, but NOT a trial start of version {version}: nothing confirmed.")
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["restart", "wait", "confirm"])
    ap.add_argument("what", nargs="?", help="restart: the command to run; confirm: the version that was pushed")
    ap.add_argument("--host", default="192.168.1.185")
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519")
    ap.add_argument("--known-hosts", default=str(ROOT / "assets" / "local" / "dell_known_hosts"))
    ap.add_argument("--wait", type=int, default=900, help="how long to wait for the restart, seconds (default 900)")
    ap.add_argument("--dry", action="store_true", help="confirm: everything but the confirmation")
    args = ap.parse_args(argv)
    if args.mode in ("restart", "confirm") and not args.what:
        ap.error(f"{args.mode} needs an argument")
    m = Machine(args)
    token = plant(m)
    log(f"baseline: token {token} planted")
    t0 = time.time()
    if args.mode == "restart":
        log("running:", args.what)
        rc, out, err = m.ssh(args.what, timeout=12)                  # (the connection dies with the restart: a timeout or 255 is expected)
        log(f"command returned: rc={rc} out={out.strip()[:200]!r}")
        result = wait_restart(m, token, time.time() + args.wait, t_cmd=time.time(), give_up_same=45)
        return 0 if result == "restarted" else 1 if result == "same" else 4
    result = wait_restart(m, token, t0 + args.wait)
    if result != "restarted":
        log("gave up waiting")
        return 4
    return 0 if args.mode == "wait" else confirm_trial(m, args.what, args.dry)


if __name__ == "__main__":
    sys.exit(main())
