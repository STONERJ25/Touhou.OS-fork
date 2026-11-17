#!/usr/bin/env python3
"""Runs the checks of the Linux personality, the Linux root and the machine's own state on a real TouhouOS machine over ssh, and writes a report.

  python tools/dell-check.py --host ADDRESS --fingerprint SHA256:...            all stages
  python tools/dell-check.py --host 127.0.0.1 --port 2222 --trust-first-use      a QEMU guest with sshd (its host key changes at every start)
  python tools/dell-check.py --host H --fingerprint F --only state,free,static   some stages (state free static root scripts dynamic volume)
  python tools/dell-check.py --list                                              what would run, nothing else

It is the real-machine twin of tests/case_linux*.py: the same programs, the same arguments, the same stored outputs of a real Linux (tests/linux/golden), but the machine is
reached over ssh as `marisa` (docs/SSH.md, the dev key, sudo) and the outputs come back through a pipe, which is exactly how tools/linux-oracle.py captured them. The programs come
from build/linux-inputs (python tools/make-linux-root.py and `wsl.exe -e python3 tools/linux-oracle.py --inputs` make them); the machine must be running a build that has the
Linux personality. The stages, in the order of rising risk:

  state     read-only: uname, /proc/cmdline, update status, watchdog, fatcheck, usbstorage, DHCP, memory, the whole kernel log (kept in the report)
  free      the freestanding programs of the initrd (user/linuxtest): hello, exit statuses, args, a probe of fifty-odd calls
  static    the statically linked glibc programs (files, directories, signals, threads, real-time signals, timers, cancel, set-id, SIGSYS, sockets over loopback)
  root      `linuxroot` unpacks the Linux root into a tmpfs on /linux; ls, cat, a shell
  scripts   three shell scripts run by dash and by bash in the root, compared with a real Linux
  dynamic   the dynamically linked programs (PIE, non-PIE, static-PIE, dlopen, spawn, the C library tour again) from the root
  volume    the root's archive is copied onto the Human Village volume (a real FAT write) and read back

The report goes to docs/logs/ (not published: logs of the real machine). The host key is pinned: the tool refuses a machine whose ed25519 key does not have the fingerprint given
with --fingerprint (--trust-first-use is for a QEMU guest only). Only the standard library and the ssh, scp, ssh-keyscan and ssh-keygen clients are needed.
Exit status: 0 everything compared equal, 1 a difference or a failure, 2 bad arguments or the machine could not be reached.
"""
import argparse
import difflib
import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "linux" / "golden"
INPUTS = ROOT / "build" / "linux-inputs"
SCRIPTS_DIR = ROOT / "tests" / "linux" / "scripts"
LOGS = ROOT / "docs" / "logs"
TESTS_DIR = "/ScarletDevilMansion/share/linux-tests"
STAGES = ["state", "free", "static", "root", "scripts", "dynamic", "volume"]
SELECTED = None                                         # --programs: only these programs (None: all)
DEFAULT_LIMIT = 300                                     # seconds per program (the QEMU tests allow up to 300 for the slow ones)
STATE_COMMANDS = ["uname -a", "cat /proc/cmdline", "sudo update status", "cat /proc/watchdog", "cat /proc/fatcheck", "cat /proc/usbstorage", "cat /proc/net/dhcp",
                  "cat /proc/meminfo", "ifconfig", "ls -l /HumanVillage/.system/etc"]


def load_oracle():
    spec = importlib.util.spec_from_file_location("linux_oracle", ROOT / "tools" / "linux-oracle.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def wanted(name):
    return SELECTED is None or name in SELECTED


def env_prefix(env):
    return "".join(f"{k}={v} " for k, v in env.items())


class Report:
    def __init__(self, path):
        self.path = path
        self.lines = []
        self.passed, self.failed = [], []

    def say(self, text="", echo=True):
        self.lines.append(text)
        if echo:
            print(text, flush=True)

    def section(self, title):
        self.say(f"\n=== {title}")

    def result(self, name, ok, detail="", seconds=None):
        (self.passed if ok else self.failed).append(name)
        t = f"  {seconds:.1f} s" if seconds is not None else ""
        self.say(f"{'PASS' if ok else 'FAIL'} {name}{t}")
        if detail and not ok:
            self.say(detail)

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text("\n".join(self.lines) + "\n", encoding="utf-8", newline="\n")


class Machine:
    """The machine over ssh: run() a command (stdout and stderr apart, no input), put() files into its /tmp."""

    def __init__(self, a, known_hosts):
        self.host, self.user, self.port = a.host, a.user, str(a.port)
        self.opts = ["-i", a.key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", f"UserKnownHostsFile={Path(known_hosts).resolve().as_posix()}",
                     "-o", "StrictHostKeyChecking=" + ("accept-new" if a.trust_first_use else "yes"), "-o", "ConnectTimeout=25", "-o", "ServerAliveInterval=20", "-q"]
        self.ssh, self.scp = a.ssh, a.scp

    def run(self, cmd, timeout=120):
        try:
            p = subprocess.run([self.ssh, *self.opts, "-p", self.port, "-n", f"{self.user}@{self.host}", cmd], capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "", f"(no answer within {timeout} s)"
        return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r", ""), p.stderr.decode("utf-8", "replace").replace("\r", "")

    def put(self, files, remote_dir="/tmp", timeout=600):
        cmd = [self.scp, *self.opts, "-P", self.port, *[str(f) for f in files], f"{self.user}@{self.host}:{remote_dir}/"]
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, f"scp: no answer within {timeout} s"
        return p.returncode == 0, p.stderr.decode("utf-8", "replace")


def pin_host_key(a, path):
    """Writes known_hosts with the machine's ed25519 key after checking its fingerprint against --fingerprint (or accepting it, with --trust-first-use)."""
    if a.trust_first_use:
        return True, "host key accepted on first use (a QEMU guest)"
    if not a.fingerprint:
        return False, "give --fingerprint SHA256:... (the pinned ed25519 host key), or --trust-first-use for a QEMU guest"
    host = a.host if a.port == 22 else f"[{a.host}]:{a.port}"
    scan = subprocess.run([a.ssh_keyscan, "-p", str(a.port), "-t", "ed25519", a.host], capture_output=True, timeout=60)
    keys = [l for l in scan.stdout.decode("utf-8", "replace").splitlines() if l and not l.startswith("#")]
    if not keys:
        return False, "ssh-keyscan got no ed25519 key from the machine (is it up? is sshd running?)"
    Path(path).write_text("\n".join(keys) + "\n", encoding="utf-8", newline="\n")
    fp = subprocess.run([a.ssh_keygen, "-lf", str(path)], capture_output=True, timeout=30).stdout.decode("utf-8", "replace")
    if a.fingerprint not in fp:
        return False, f"the host key of {host} is {fp.strip()!r}, not {a.fingerprint}: refusing to go on"
    return True, f"host key {a.fingerprint} (pinned)"


def compare(name, out, rc):
    """None when the machine printed what the stored output of a real Linux says and ended with the same status, else the difference."""
    want = (GOLDEN / f"{name}.txt").read_text(encoding="utf-8")
    got = out + f"exit status: {rc}\n"
    if got == want:
        return None
    return "\n".join(difflib.unified_diff(want.splitlines(), got.splitlines(), "real Linux", "this machine", lineterm="", n=1))


def run_compared(m, rep, name, cmd, timeout):
    t0 = time.time()
    rc, out, err = m.run("cd /tmp && " + cmd, timeout)        # from /tmp, as the QEMU tests ran from a home on tmpfs: a FAT home has no symbolic links, hard links or modes
    dt = time.time() - t0
    if rc == 124 and not out:
        rep.result(name, False, f"      {err}", dt)
        return False
    diff = compare(name, out, rc)
    rep.result(name, diff is None, "\n".join("      " + l for l in (diff or "").splitlines()[:40]) + (f"\n      stderr: {err.strip()[:300]}" if err.strip() and diff else ""), dt)
    return diff is None


def stage_state(m, rep, a):
    rep.section("state")
    for cmd in STATE_COMMANDS:
        rc, out, err = m.run(cmd, 60)
        rep.say(f"$ {cmd}   (exit {rc})\n{out.rstrip()}" + (f"\n[stderr] {err.rstrip()}" if err.strip() else ""))
    rc, out, err = m.run("dmesg", 120)
    rep.say(f"\n--- kernel log ({len(out.splitlines())} lines)\n{out.rstrip()}", echo=False)
    rep.say(f"(the kernel log, {len(out.splitlines())} lines, is in the report file)")
    return out


def stage_free(m, rep, orc):
    rep.section("free: the freestanding programs of the initrd")
    for n, (args, env) in orc.FREE_PROGRAMS.items():
        if wanted(n):
            run_compared(m, rep, n, f"{env_prefix(env)}linux -r {TESTS_DIR}/{n} {' '.join(args)}".strip(), 180)
    for arg, want in (("", 42), ("3", 3)) if wanted("lnx_exit") else ():
        rc, out, err = m.run(f"cd /tmp && linux -r {TESTS_DIR}/lnx_exit {arg}".strip(), 60)
        rep.result(f"lnx_exit {arg}".strip() + f" ends with status {want}", rc == want, f"      status {rc}, output {out!r}")


def push_programs(m, rep, names):
    paths = [INPUTS / n for n in names if (INPUTS / n).is_file()]
    missing = [n for n in names if not (INPUTS / n).is_file()]
    if missing:
        rep.say(f"(not built, skipped: {', '.join(missing)}; run wsl.exe -e python3 tools/linux-oracle.py --inputs)")
    if paths:
        ok, err = m.put(paths)
        if not ok:
            rep.say(f"scp failed: {err}")
            return []
        m.run("chmod +x " + " ".join(f"/tmp/{p.name}" for p in paths), 60)
    return [p.name for p in paths]


def stage_static(m, rep, orc):
    rep.section("static: statically linked glibc programs")
    names = [n for n in orc.HOSTED_PROGRAMS if not n.startswith("dyn_") and n != "map_tour" and n not in orc.NO_RUN and wanted(n)]
    have = push_programs(m, rep, names)
    for n in names:
        if n in have:
            args, env = orc.HOSTED_PROGRAMS[n]
            run_compared(m, rep, n, f"{env_prefix(env)}linux -r /tmp/{n} {' '.join(args)}".strip(), DEFAULT_LIMIT)


def stage_root(m, rep):
    rep.section("root: the Linux root")
    tar = INPUTS / "linux-root.tar"
    if not tar.is_file():
        rep.result("linux-root.tar exists", False, "      python tools/make-linux-root.py")
        return False
    ok, err = m.put([tar])
    rep.result("copy linux-root.tar to /tmp", ok, f"      {err}")
    if not ok:
        return False
    t0 = time.time()
    rc, out, err = m.run("sudo linuxroot /tmp/linux-root.tar 2>&1", 300)
    rep.result("linuxroot unpacks the archive", rc == 0, f"      status {rc}: {out.strip()[:400]}", time.time() - t0)
    if rc:
        return False
    rep.say("  " + out.strip())
    rc, out, _ = m.run("linux /bin/ls /", 120)
    rep.result("linux /bin/ls /", rc == 0 and "usr" in out.split() and "lib64" in out.split(), f"      status {rc}: {out[:300]!r}")
    rc, out, _ = m.run("linux /bin/cat /etc/os-release", 60)
    rep.result("linux /bin/cat /etc/os-release", rc == 0 and out.startswith('PRETTY_NAME="Ubuntu'), f"      status {rc}: {out[:200]!r}")
    rc, out, _ = m.run("linux /bin/sh -c 'echo $((1+2))'", 60)
    rep.result("linux /bin/sh -c 'echo $((1+2))'", rc == 0 and out == "3\n", f"      status {rc}: {out!r}")
    rc, out, _ = m.run("linux /usr/bin/id", 60)
    rep.say(f"  id: {out.strip()}  (status {rc})")
    rc, out, _ = m.run("linux /usr/bin/uname -a", 60)
    rep.say(f"  uname: {out.strip()}  (status {rc})")
    return True


def stage_scripts(m, rep, orc):
    rep.section("scripts: dash and bash in the Linux root")
    scripts = [SCRIPTS_DIR / s for s in orc.SCRIPTS.values()]
    ok, err = m.put(scripts)
    if not ok:
        rep.result("copy the scripts", False, f"      {err}")
        return
    for shell in ("/bin/sh", "/bin/bash"):
        for n, script in orc.SCRIPTS.items():
            if not wanted(n):
                continue
            cmd = f"rm -rf /tmp/wk; mkdir /tmp/wk; (cd /tmp/wk && linux /usr/bin/env -i PATH=/usr/bin:/bin {shell} /tmp/{script} 2>&1 | cat)"
            t0 = time.time()
            rc, out, err = m.run(cmd, 240)
            diff = compare(n, out, rc)
            rep.result(f"{n} ({shell})", diff is None, "\n".join("      " + l for l in (diff or "").splitlines()[:40]), time.time() - t0)


def stage_dynamic(m, rep, orc):
    rep.section("dynamic: dynamically linked programs from the root")
    names = [n for n in orc.HOSTED_PROGRAMS if (n.startswith("dyn_") or n == "map_tour") and wanted(n)]
    lib = INPUTS / "libdynplug.so"
    have = push_programs(m, rep, names)
    if lib.is_file():
        m.put([lib])
    for n in names:
        if n not in have:
            continue
        args, env = orc.HOSTED_PROGRAMS[n]
        env = {k: ("/tmp" if v == "@INPUTS@" else v) for k, v in env.items()}
        run_compared(m, rep, n, f"{env_prefix(env)}linux /tmp/{n} {' '.join(args)}".strip(), DEFAULT_LIMIT)


def stage_volume(m, rep):
    rep.section("volume: the archive onto the Human Village (a FAT write) and back")
    tar = INPUTS / "linux-root.tar"
    if not tar.is_file():
        rep.result("linux-root.tar exists", False)
        return
    want = hashlib.sha256(tar.read_bytes()).hexdigest()
    ok, err = m.put([tar])
    if not ok:
        rep.result("copy linux-root.tar to /tmp", False, f"      {err}")
        return
    t0 = time.time()
    rc, out, err = m.run("sudo cp /tmp/linux-root.tar /HumanVillage/linux-root.tar && sync && ls -l /HumanVillage/linux-root.tar", 300)
    rep.result("cp to /HumanVillage and sync", rc == 0, f"      status {rc}: {out.strip()} {err.strip()}", time.time() - t0)
    if rc:
        return
    rc, out, err = m.run("sha256sum /HumanVillage/linux-root.tar", 120)
    got = out.split()[0] if out.split() else ""
    rep.result("the copy on the volume has the archive's sha256", got == want, f"      wanted {want}, got {got or err.strip()!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--user", default="marisa")
    ap.add_argument("--key", default=str(Path.home() / ".ssh" / "touhou_ed25519"))
    ap.add_argument("--fingerprint", help="the pinned ed25519 host key, SHA256:...")
    ap.add_argument("--trust-first-use", action="store_true", help="accept whatever host key the machine shows (a QEMU guest only)")
    ap.add_argument("--known-hosts", help="where the pinned key is written (default: a temporary file)")
    ap.add_argument("--only", help="comma-separated stages: " + " ".join(STAGES))
    ap.add_argument("--report", help="the report file (default: docs/logs/real-machine-DATE.log)")
    ap.add_argument("--programs", help="comma-separated program names: only these (lnx_hello, glibc_sigsys, dyn_hello, sh_files ...)")
    ap.add_argument("--list", action="store_true", help="show what would run and stop")
    ap.add_argument("--ssh", default="ssh")
    ap.add_argument("--scp", default="scp")
    ap.add_argument("--ssh-keyscan", default="ssh-keyscan")
    ap.add_argument("--ssh-keygen", default="ssh-keygen")
    a = ap.parse_args()
    stages = [s for s in (a.only.split(",") if a.only else STAGES) if s]
    bad = [s for s in stages if s not in STAGES]
    if bad:
        print(f"unknown stage(s): {', '.join(bad)}; the stages are {' '.join(STAGES)}", file=sys.stderr)
        return 2
    global SELECTED
    SELECTED = set(a.programs.split(",")) if a.programs else None
    orc = load_oracle()
    if a.list:
        print("stages:", " ".join(stages))
        print("freestanding:", " ".join(orc.FREE_PROGRAMS))
        print("static glibc:", " ".join(n for n in orc.HOSTED_PROGRAMS if not n.startswith("dyn_") and n != "map_tour" and n not in orc.NO_RUN))
        print("dynamic:", " ".join(n for n in orc.HOSTED_PROGRAMS if n.startswith("dyn_") or n == "map_tour"))
        print("scripts:", " ".join(orc.SCRIPTS))
        return 0
    if not a.host:
        ap.error("--host is required")
    if not os.path.isfile(a.key):
        print(f"the key {a.key} does not exist", file=sys.stderr)
        return 2
    report_path = Path(a.report) if a.report else LOGS / f"real-machine-{time.strftime('%Y-%m-%d')}.log"
    rep = Report(report_path)
    tmp = None
    known = a.known_hosts
    if not known:
        tmp = tempfile.NamedTemporaryFile(prefix="dell-known-hosts-", suffix=".txt", delete=False)
        tmp.close()
        known = tmp.name
    try:
        ok, why = pin_host_key(a, known)
        rep.say(f"dell-check: {time.strftime('%Y-%m-%d %H:%M:%S')}, machine {a.host}:{a.port}, stages {' '.join(stages)}; {why}")
        if not ok:
            return 2
        m = Machine(a, known)
        rc, out, err = m.run("echo up", 60)
        if rc != 0 or out.strip() != "up":
            rep.say(f"cannot log in as {a.user}@{a.host}: status {rc}, {err.strip()[:300]}")
            return 2
        t_start = time.time()
        log_before = ""
        for s in stages:
            if s == "state":
                log_before = stage_state(m, rep, a)
            elif s == "free":
                stage_free(m, rep, orc)
            elif s == "static":
                stage_static(m, rep, orc)
            elif s == "root":
                if not stage_root(m, rep):
                    rep.say("(the root did not come up: the stages scripts and dynamic are skipped)")
                    stages = [x for x in stages if x not in ("scripts", "dynamic")]
            elif s == "scripts":
                stage_scripts(m, rep, orc)
            elif s == "dynamic":
                stage_dynamic(m, rep, orc)
            elif s == "volume":
                stage_volume(m, rep)
            rep.save()
        rc, log_after, _ = m.run("dmesg", 120)
        if rc == 0:
            new = log_after.splitlines()[len(log_before.splitlines()):] if log_before else log_after.splitlines()
            flagged = [l for l in new if any(w in l for w in ("panic", "Panic", "PANIC", "Outside World", "was exorcised", "BUG", "oops", "Oops"))]
            rep.section("kernel log lines of this run that deserve a look")
            rep.say("\n".join(flagged[:60]) if flagged else "(none)")
            rep.say(f"\n--- kernel log at the end ({len(log_after.splitlines())} lines)\n{log_after.rstrip()}", echo=False)
        rep.section("summary")
        rep.say(f"{len(rep.passed)} passed, {len(rep.failed)} failed, {time.time() - t_start:.0f} s")
        if rep.failed:
            rep.say("failed: " + ", ".join(rep.failed))
        rep.say(f"report: {report_path}")
        rep.save()
        return 1 if rep.failed else 0
    finally:
        rep.save()
        if tmp:
            try:
                os.unlink(tmp.name)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
