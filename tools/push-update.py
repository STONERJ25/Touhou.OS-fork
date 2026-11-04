#!/usr/bin/env python3
"""Push a new kernel and initrd to a running TouhouOS over ssh, restart it, and confirm that it came up (docs/UPDATE.md).

  python tools/push-update.py --host 192.168.1.20                  build/esp -> the machine; restarts it; waits; confirms
  python tools/push-update.py --host gensokyo --no-reboot          install only: the machine restarts later (on trial, see below)
  python tools/push-update.py --host H --loader                    also replace the loader (EFI/BOOT/BOOTX64.EFI)
  python tools/push-update.py --host H --cmdline "text blk.ro"     also replace cmdline.txt (the whole line)
  python tools/push-update.py --host H --dry-run                   show what would be sent

The machine runs `sudo update recv` (the `update` applet), which checks the hashes, writes the ESP so that a power cut leaves every file
whole, keeps the previous kernel and initrd as kernel.old and initrd.old and puts the new files on trial. This script then waits until
the machine has restarted (a token it left in the machine's /tmp, which is RAM, is gone: it logs in to look, it does not poll port 22, which the
machine's sshd answers with a minute of silence after five connections without a login) and runs `sudo update confirm`, but only when it reports a start on
trial AND the version it installed is the one sent here (a machine that came back in some other state is reported, not confirmed); if nobody
confirms, the machine restarts by itself after five minutes and, after a second failed start, the loader brings the previous files back.
Exit status: 0 done, 1 the machine refused or ssh failed, 2 bad arguments, 3 the machine came back on the PREVIOUS files (the new ones did not
come up), 4 it did not restart or did not come back in time.
Ctrl+C here does not interrupt the update on the machine: the part that writes the ESP runs there in a session of its own and finishes.

The login uses the key ~/.ssh/touhou_ed25519 by default (docs/SSH.md). Only the standard library and the ssh client are needed.

initrd.img is a gzip member by default now (docs/UPDATE.md, "The compressed initrd"): this tool unpacks it to look at the key list and refuses one that the kernel would refuse (damaged, cut
short, bytes after the end, more than 256 MiB unpacked) before it sends anything. A machine whose `update` applet predates that change refuses a gzip initrd ("is not a tar archive"): the push
that brings it the new kernel has to be built with TOUHOU_INITRD=plain python tools/build.py.
"""
import argparse
import hashlib
import os
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = ["kernel.elf", "initrd.img", "cmdline.txt", "EFI/BOOT/BOOTX64.EFI"]


def build_stream(files, version):
    """The bytes of a push: header lines, then the files in the order of the header. files: a list of (name, bytes)."""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}", version):
        raise ValueError(f"bad version {version!r}: letters, digits and ._+- only, at most 64 characters")
    head = ["touhou-push 1", f"version {version}"]
    for name, data in files:
        head.append(f"file {name} {len(data)} {hashlib.sha256(data).hexdigest()}")
    head.append("end")
    return ("\n".join(head) + "\n").encode("ascii") + b"".join(data for _, data in files)


def git_version():
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty", "--tags"], cwd=ROOT, capture_output=True, text=True, timeout=20).stdout.strip()
        if out:
            return re.sub(r"[^A-Za-z0-9._+-]", "-", out)[:64] or "unknown"
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


class Ssh:
    def __init__(self, args):
        exe = shutil.which(args.ssh) or args.ssh
        key = Path(args.key).expanduser()
        self.base = [exe, "-p", str(args.port), "-i", str(key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                     "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15",
                     "-o", "ServerAliveCountMax=8", "-q"]
        for o in args.ssh_option:
            self.base += ["-o", o]
        self.target = f"{args.user}@{args.host}"
        # a Ctrl+C at this terminal must not reach the ssh client (and so cut the connection) in the middle of an update: the script decides what to do with it
        self.flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.send_timeout = args.send_timeout

    def run(self, command, stdin=None, timeout=120):
        """(exit status, stdout, stderr); the status is None when ssh did not finish in time (it is stopped)."""
        p = subprocess.Popen(self.base + [self.target, command], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=self.flags)
        try:
            out, err = p.communicate(stdin, timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            return None, b"", b""
        return p.returncode, out, err

    def stream(self, command, data, echo):
        """Runs command with data on its standard input, printing its output lines as they come. Returns (exit status, all output); the status is None on a timeout."""
        p = subprocess.Popen(self.base + [self.target, command], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=self.flags)
        lines = []

        def pump():
            for raw in p.stdout:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                lines.append(line)
                echo(line)

        t = threading.Thread(target=pump, daemon=True)
        t.start()

        def feed():
            try:
                p.stdin.write(data)
                p.stdin.close()
            except OSError:
                pass                                # the machine refused early and said why; its output is what matters

        f = threading.Thread(target=feed, daemon=True)
        f.start()                                   # (a writer of its own: a stalled connection must not make the timeout below unreachable)
        try:
            p.wait(timeout=self.send_timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            t.join(5)
            return None, "\n".join(lines)
        t.join(10)
        f.join(2)
        return p.returncode, "\n".join(lines)


TOKEN = "/tmp/.push-update-token"


def plant_token(ssh):
    """Leaves a token in the machine's /tmp, which is RAM and empty at every start: when a login finds the token gone, the machine has restarted since. Neither the state of
    port 22 nor the uptime proves that: the machine's sshd turns an address away for a minute after five connections that ended without a login, so a script that polled
    the port made a running machine look down and took the old start for the new one (2026-10-25); and the uptime of a virtual machine on a busy host lags behind real time.
    Returns the token, or None if the machine did not take it."""
    token = secrets.token_hex(8)
    rc, out, _ = ssh.run(f"echo {token} > {TOKEN}; cat {TOKEN}; echo END", timeout=60)
    return token if rc == 0 and token in out.decode("utf-8", "replace") else None


def look_at_token(rc, out, token):
    """What a login says about the token: "same" (it is there: the start it was planted in), "new" (the login works and the token is gone: a new start), None (no answer)."""
    text = out.decode("utf-8", "replace") if isinstance(out, (bytes, bytearray)) else out
    if rc != 0 or "END" not in text:
        return None
    return "same" if token in text else "new"


UNPACKED_MAX = 256 << 20                         # what the kernel accepts a gzip initrd to unpack to (kernel/fs/initrd.c)


def initrd_tar(initrd):
    """The ustar archive inside an initrd.img: the bytes themselves, or what they unpack to when they are a gzip stream (docs/UPDATE.md, "The compressed initrd"). A gzip stream is
    held to what the kernel holds it to: one member, CRC-32 and length right, nothing after it, at most 256 MiB unpacked. Raises ValueError (with the reason) when it is not."""
    import zlib
    if initrd[:2] != b"\x1f\x8b":
        return initrd
    d = zlib.decompressobj(wbits=31)
    try:
        out = d.decompress(initrd, UNPACKED_MAX + 1)
    except zlib.error as e:
        raise ValueError(f"the gzip stream is damaged ({e})") from None
    if len(out) > UNPACKED_MAX:
        raise ValueError("the gzip stream unpacks to more than 256 MiB, which the kernel refuses")
    if not d.eof:
        raise ValueError("the gzip stream ends too early")
    if d.unused_data:
        raise ValueError(f"{len(d.unused_data)} bytes follow the end of the gzip stream (a single member is the format; the kernel refuses anything after it)")
    return out


def authorized_blobs(initrd):
    """The key blobs (the base64 field of each line) that the image's /etc/ssh/authorized_keys files let in; None if the initrd is not a tar archive that can be read
    (a gzip one is unpacked first)."""
    import io
    import tarfile
    try:
        out = set()
        with tarfile.open(fileobj=io.BytesIO(initrd_tar(initrd))) as t:
            for m in t:
                if m.isfile() and m.name.startswith("Hakurei/etc/ssh/authorized_keys/"):
                    for line in t.extractfile(m).read().decode("utf-8", "replace").splitlines():
                        f = line.split()
                        if len(f) >= 2 and not line.lstrip().startswith("#"):
                            out.add(f[1])
        return out
    except (tarfile.TarError, EOFError, OSError, ValueError):
        return None


def public_blob(key_path):
    """The base64 field of the public key that belongs to the private key at key_path (KEY.pub, else ssh-keygen -y); None if neither is available."""
    p = Path(str(key_path) + ".pub")
    try:
        if p.is_file():
            f = p.read_text().split()
            return f[1] if len(f) >= 2 else None
        r = subprocess.run(["ssh-keygen", "-y", "-P", "", "-f", str(key_path)], capture_output=True, text=True, timeout=20, stdin=subprocess.DEVNULL)
        f = r.stdout.split()
        return f[1] if r.returncode == 0 and len(f) >= 2 else None
    except (OSError, subprocess.SubprocessError):
        return None


def main(argv=None):
    try:
        return run(argv)
    except KeyboardInterrupt:
        print("\npush-update: interrupted. The update on the machine is not: it finishes by itself (`sudo update status` shows how it went; the machine restarts if\n"
              "nobody confirms a start on trial).", file=sys.stderr)
        return 130


def run(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the machine: an address or a name")
    ap.add_argument("--user", default="marisa", help="the account to log in as (it must be allowed to use sudo; default marisa)")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--key", default="~/.ssh/touhou_ed25519", help="the private key (default ~/.ssh/touhou_ed25519)")
    ap.add_argument("--ssh", default="ssh", help="the ssh client (default: ssh from the PATH)")
    ap.add_argument("--ssh-option", action="append", default=[], metavar="OPT", help="an extra ssh -o option, e.g. UserKnownHostsFile=FILE (repeatable)")
    ap.add_argument("--src", default=str(ROOT / "build" / "esp"), help="where kernel.elf and initrd.img are (default build/esp)")
    ap.add_argument("--version", default=None, help="shown by `update status` (default: git describe)")
    ap.add_argument("--cmdline", default=None, metavar="TEXT", help="also replace cmdline.txt with this whole line (careful: it replaces the installed one)")
    ap.add_argument("--cmdline-file", default=None, metavar="FILE", help="also replace cmdline.txt with this file")
    ap.add_argument("--loader", action="store_true", help="also replace the loader, EFI/BOOT/BOOTX64.EFI (a broken loader cannot fall back to anything)")
    ap.add_argument("--no-trial", action="store_true", help="no trial: the new files are the installed system at once (the previous ones are still kept)")
    ap.add_argument("--no-reboot", action="store_true", help="install only; the machine restarts when it is restarted (on trial, unless --no-trial)")
    ap.add_argument("--no-confirm", action="store_true", help="restart and wait, but do not confirm: the machine restarts itself after 5 minutes")
    ap.add_argument("--replace-trial", action="store_true", help="an earlier update is still on trial: put this one over it (the previous set that is kept stays the one from before the earlier update)")
    ap.add_argument("--force", action="store_true", help="mount the ESP although it was not cleanly unmounted (a power cut), and go on")
    ap.add_argument("--send-timeout", type=int, default=900, help="how long the transfer and the writing on the machine may take, seconds (default 900)")
    ap.add_argument("--wait", type=int, default=420, help="how long to wait for the machine to come back, seconds (default 420)")
    ap.add_argument("--no-key-check", action="store_true", help="do not check that the initrd authorizes the key this tool logs in with (an image built in a checkout without assets/local/ssh has no keys: the machine could not be reached again)")
    ap.add_argument("--dry-run", action="store_true", help="show what would be sent and stop")
    args = ap.parse_args(argv)

    src = Path(args.src)
    files = []
    for name in ("kernel.elf", "initrd.img"):
        p = src / name
        if not p.is_file():
            print(f"push-update: {p} is missing (python tools/build.py makes it)", file=sys.stderr)
            return 2
        files.append((name, p.read_bytes()))
    if args.cmdline is not None and args.cmdline_file:
        print("push-update: --cmdline and --cmdline-file together?", file=sys.stderr)
        return 2
    if args.cmdline is not None:
        files.append(("cmdline.txt", (args.cmdline.strip() + "\n").encode("ascii")))
    elif args.cmdline_file:
        files.append(("cmdline.txt", Path(args.cmdline_file).read_bytes()))
    if args.loader:
        p = src / "EFI" / "BOOT" / "BOOTX64.EFI"
        if not p.is_file():
            print(f"push-update: {p} is missing", file=sys.stderr)
            return 2
        files.append(("EFI/BOOT/BOOTX64.EFI", p.read_bytes()))
    try:
        tar = initrd_tar(dict(files)["initrd.img"])
    except ValueError as e:                       # (the machine would refuse it too, after receiving all of it; the kernel would panic over it if it ever got that far)
        print(f"push-update: initrd.img is unusable: {e}. Nothing was sent.", file=sys.stderr)
        return 2
    if not args.no_key_check:
        mine = public_blob(Path(args.key).expanduser())
        have = authorized_blobs(tar)
        if mine and have is not None and mine not in have:
            print(f"push-update: the initrd authorizes none of the keys this tool logs in with ({args.key}): a machine started with it could not be logged into again, so the update could not\n"
                  f"be confirmed and the machine would fall back after two unconfirmed starts. The image was probably built in a checkout without assets/local/ssh/*.pub (a git worktree has none):\n"
                  f"build in the main checkout, or give --no-key-check if this is meant.", file=sys.stderr)
            return 2
    version = args.version or git_version()
    try:
        stream = build_stream(files, version)
    except ValueError as e:
        print(f"push-update: {e}", file=sys.stderr)
        return 2
    print(f"push-update: version {version}, {len(files)} files, {len(stream)} bytes for {args.user}@{args.host}")
    for name, data in files:
        note = f"  (gzip, unpacks to {len(tar)} bytes: {100 * len(data) / max(1, len(tar)):.1f}%)" if name == "initrd.img" and tar is not data else ""
        print(f"  {name:22} {len(data):>10} bytes  {hashlib.sha256(data).hexdigest()[:16]}{note}")
    if args.dry_run:
        return 0

    ssh = Ssh(args)
    token = None
    if not args.no_reboot:                       # the restart is proved by a token in /tmp being gone: it has to be there first
        token = plant_token(ssh)
        if token is None:
            print(f"push-update: the machine did not take a token in {TOKEN} (a login that writes a file in /tmp): nothing was sent. The restart is proved by the token being gone.",
                  file=sys.stderr)
            return 1
    cmd = "sudo update recv" + ("" if args.no_reboot else " --reboot=3") + (" --loader" if args.loader else "") + \
          (" --no-trial" if args.no_trial else "") + (" --replace-trial" if args.replace_trial else "") + (" --force" if args.force else "")
    t0 = time.time()
    rc, out = ssh.stream(cmd, stream, lambda line: print("  " + line, flush=True))
    if rc is None:
        print(f"push-update: the transfer did not finish within {args.send_timeout} seconds; ssh was stopped. The machine finishes what it had begun: `sudo update status`\n"
              "shows where it stands (an update that was cut off before the ESP was touched changes nothing).", file=sys.stderr)
        return 1
    if rc != 0 or "update: done" not in out:
        print(f"push-update: the machine did not take the update (status {rc})", file=sys.stderr)
        return 1
    if "update: result: unchanged" in out:
        print("push-update: nothing was changed on the machine (everything was installed already).")
        return 0
    if "update: result: installed" not in out:
        print("push-update: the machine said \"done\" without saying what it did; not going on. `sudo update status` on the machine shows the state.", file=sys.stderr)
        return 1
    if args.no_reboot:
        print("push-update: installed. The machine runs the new files after its next restart" + ("." if args.no_trial else ", on trial: `sudo update confirm` then."))
        return 0
    if "restarting in" not in out:
        print("push-update: installed, but the machine did not say it would restart; not waiting for it. Restart it, or look at `sudo update status`.", file=sys.stderr)
        return 1

    # the machine restarts: it has when a login finds the token gone (and only then is what `update status` says about "this start" about the new start)
    print("push-update: waiting for the machine to restart ...", flush=True)
    deadline = time.time() + args.wait
    status = None
    still_old = False
    while time.time() < deadline:
        rc, out, _ = ssh.run(f"cat {TOKEN} 2>&1; echo END", timeout=15)
        seen = look_at_token(rc, out, token)
        if seen == "new":
            still_old = False
            rc, so, _ = ssh.run("sudo update status", timeout=60)
            if rc == 0 and b"this start" in so:
                status = so.decode("utf-8", "replace")
                break
        else:
            still_old = seen == "same"           # it answers, and the token is still there: the old start
        time.sleep(3)
    if status is None:
        if still_old:
            print("push-update: the machine never restarted (the token is still in its /tmp): did the restart fail? `sudo update status` shows how it stands; the files are\n"
                  "installed and on trial, and nothing has proved them.", file=sys.stderr)
        else:
            print(f"push-update: the machine did not come back within {args.wait} seconds. It restarts itself five minutes after each start that is not\n"
                  "confirmed and, after the second, the loader starts the previous kernel and initrd.", file=sys.stderr)
        return 4
    print(f"push-update: the machine is back after {int(time.time() - t0)} seconds:")
    for line in status.splitlines():
        print("  " + line)
    if "PREVIOUS" in status:
        print("push-update: it came back on the PREVIOUS files: the new ones did not come up in time. `sudo update rollback` makes that permanent.", file=sys.stderr)
        return 3
    if args.no_trial or args.no_confirm:
        print("push-update: not confirming." + ("" if args.no_trial else " `sudo update confirm` within five minutes, or the machine restarts."))
        return 0
    m = re.search(r"installed by the last push: version (\S+?),", status)
    if "this start: the new files, on trial" not in status or not m or m.group(1) != version:
        print(f"push-update: the machine is up, but it is not on a trial start of version {version} (it reports version {m.group(1) if m else 'none'}); not confirming. "
              "`sudo update status` and `sudo update confirm` on the machine are yours to run.", file=sys.stderr)
        return 1
    rc, so, se = ssh.run("sudo update confirm", timeout=60)
    sys.stdout.write("  " + so.decode("utf-8", "replace").replace("\n", "\n  ").rstrip(" ") if so else "")
    if rc != 0:
        print(f"push-update: the confirmation failed: {se.decode('utf-8', 'replace').strip() or 'ssh did not answer'}", file=sys.stderr)
        return 1
    print("push-update: done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
