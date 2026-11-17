#!/usr/bin/env python3
"""Runs programs inside a TouhouOS guest and tells how each one ended: its output, its exit code, the signal that killed it, a time-out.

  python tools/run-in-guest.py [options] PROGRAM [ARGUMENT...]       one program: its output goes to stdout, its status becomes ours
  python tools/run-in-guest.py [options] --batch MANIFEST.json       many programs in one boot: one JSON line per program, in manifest order

PROGRAM is a native TouhouOS executable on this machine (a static ELF64 at 0x400000: docs/COMPAT.md). The tool boots build/esp in QEMU (a private copy, so a test run in the
same checkout is not disturbed), gives the guest the programs over QEMU's user-mode network, runs them with the guest's `runstatus` applet and reads the results back from
the serial console. Build the image first:   cd <this checkout> && python tools/build.py
It takes one of the shared QEMU slots (tools/qemu-slot.py) by itself; --no-slot if you hold one already.

THE SINGLE FORM  stdout of the tool = the program's output (standard output and standard error together, in the order they were written: the console has no separate
streams, and the guest keeps them in one file); the exit status of the tool is the program's, except:  128+N  the program was killed by signal N  /  124  it ran out of
time (--timeout)  /  125  the tool itself failed (no image, the guest did not boot, bad usage)  /  126  the program could not be started. A line on stderr says which.
--json prints the full record instead. With --split the program's standard output is the tool's standard output and its standard error is the tool's standard error.

THE BATCH FORM  MANIFEST.json is a list of programs (or {"programs": [...]}), each an object:
    "name"     a label that comes back in the result (default: the file's name)
    "file"     the executable on this machine (required)
    "args"     a list of strings: the arguments after the program's name (default none)
    "timeout"  seconds the program may run (default --timeout); a program still running then is killed with SIGKILL
    "stdin"    a file on this machine that becomes the program's standard input (default: /dev/null)
    "files"    {"relative/name": "file on this machine"}: put into the program's working directory before it runs
    "argv0"    what the program sees as argv[0] (default: the path of its copy in the guest)
Relative paths are taken from the current directory. Every program runs in a fresh working directory (made with --mkdir DIR inside, if asked), as the user reimu, with standard
input from /dev/null, one program at a time, all in one boot; the guest is restarted if a program takes it down. Standard output of the tool, one JSON object per line:
    {"name": ..., "output": "...", "exit": 3, "signal": null, "timed_out": false, "ms": 12}
  output     the program's standard output and standard error, merged, as text (invalid UTF-8 becomes U+FFFD and the exact bytes come as "output_b64")
  exit       the exit code 0..255, or null when a signal killed the program        signal  the signal number (8 = SIGFPE, 11 = SIGSEGV, 9 = SIGKILL), or null after an exit
  timed_out  true when the time limit killed it (then exit is null and signal is 9)
  ms         how long it ran, measured in the guest
  with --split "output" is replaced by "stdout" and "stderr" (each as text, with "stdout_b64" / "stderr_b64" when the bytes are not valid UTF-8): the program's two streams apart,
  which is what a runner that compares them separately needs; the order of writes between the two is not kept then (merged, as above, it is)
  and only when there is something to say:   "truncated": true (it wrote more than --max-output bytes, 256 KiB by default, and was killed)   "error": "..." (it could not be started, or the guest failed
  while it ran: no exit and no signal then)   "kernel": [...] (lines the kernel wrote to the console while it ran, such as the report of a fault)
The tool's exit status is 0 when every program was run, 1 when a guest failed (the line of the program it failed on has "error"), 2 or 125 for bad usage.
The time limits are multiplied by TOUHOU_TIMEOUT_SCALE like every timeout of the test harness. docs/RUN_IN_GUEST.md has the rest.
"""
import argparse
import base64
import binascii
import functools
import hashlib
import http.server
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SLOT_ENV = "TOUHOU_QEMU_SLOT_HELD"           # set by tools/qemu-slot.py for the command it runs

SIGNALS = {1: "SIGHUP", 2: "SIGINT", 3: "SIGQUIT", 4: "SIGILL", 5: "SIGTRAP", 6: "SIGABRT", 7: "SIGBUS", 8: "SIGFPE", 9: "SIGKILL", 10: "SIGUSR1", 11: "SIGSEGV",
           12: "SIGUSR2", 13: "SIGPIPE", 14: "SIGALRM", 15: "SIGTERM"}
GUEST_DIR = "/tmp/rig"
MAX_BOOTS = 6                                 # guests started in one run, counting the restarts after a crash
SUBDIR_OK = re.compile(r"^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$")


class ToolError(Exception):
    """The run cannot go on (bad manifest, no image, ...): the message is for the user."""


class GuestFailed(Exception):
    """The guest stopped answering or went away. started: the batch had begun, so the first program without a result is the one that did it."""

    def __init__(self, reason, started, kernel=()):
        super().__init__(reason)
        self.reason, self.started, self.kernel = reason, started, list(kernel)


# ---------------------------------------------------------------- the manifest

def wsl_to_windows(p, windows):
    """The compiler tools run in WSL and name files /mnt/c/...; Windows Python cannot open those, so on Windows they become C:/..."""
    m = re.match(r"^/mnt/([A-Za-z])(?:/(.*))?$", p) if windows else None
    return f"{m.group(1).upper()}:/{m.group(2) or ''}" if m else p


def host_path(p):
    return Path(wsl_to_windows(str(p), os.name == "nt"))


class Job:
    def __init__(self, index, name, file, args, timeout, stdin, files, argv0):
        self.index, self.name, self.file, self.args, self.timeout = index, name, file, args, timeout
        self.stdin, self.files, self.argv0 = stdin, files, argv0


def _check_str(v, what):
    if not isinstance(v, str) or "\0" in v:
        raise ToolError(f"{what} must be a string without NUL")
    return v


def parse_manifest(data, default_timeout):
    """The manifest as read from JSON (a list, or an object with "programs") -> list of Job. Raises ToolError."""
    if isinstance(data, dict):
        data = data.get("programs")
    if not isinstance(data, list) or not data:
        raise ToolError("the manifest must be a non-empty list of programs")
    jobs = []
    for i, e in enumerate(data):
        where = f"manifest entry {i + 1}"
        if not isinstance(e, dict):
            raise ToolError(f"{where}: must be an object")
        unknown = set(e) - {"name", "file", "args", "timeout", "stdin", "files", "argv0"}
        if unknown:
            raise ToolError(f"{where}: unknown key(s) {', '.join(sorted(unknown))}")
        file = _check_str(e.get("file"), f"{where}: \"file\"")
        name = _check_str(e.get("name", host_path(file).name), f"{where}: \"name\"")
        args = e.get("args", [])
        if not isinstance(args, list):
            raise ToolError(f"{where}: \"args\" must be a list of strings")
        args = [_check_str(a, f"{where}: an argument") for a in args]
        timeout = e.get("timeout", default_timeout)
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or timeout <= 0:
            raise ToolError(f"{where}: \"timeout\" must be a positive number of seconds")
        stdin = _check_str(e["stdin"], f"{where}: \"stdin\"") if "stdin" in e else None
        files = e.get("files", {})
        if not isinstance(files, dict):
            raise ToolError(f"{where}: \"files\" must be an object of name: file")
        for dest, src in files.items():
            _check_str(dest, f"{where}: a name in \"files\"")
            _check_str(src, f"{where}: a file in \"files\"")
            if not dest or dest.startswith("/") or ".." in dest.split("/"):
                raise ToolError(f"{where}: \"files\" names must be relative and stay inside the working directory ({dest!r})")
        argv0 = _check_str(e["argv0"], f"{where}: \"argv0\"") if "argv0" in e else None
        jobs.append(Job(i, name, host_path(file), args, float(timeout), host_path(stdin) if stdin is not None else None, {d: host_path(s) for d, s in files.items()}, argv0))
    return jobs


def read_manifest(path, default_timeout):
    try:
        data = json.loads(host_path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ToolError(f"{path}: {e}")
    return parse_manifest(data, default_timeout)


class Payload:
    """The files that go to the guest in one tar, and where each job finds its own in the guest."""

    def __init__(self, jobs, tar_path):
        self.paths = {}                                          # job index -> (program path, stdin path or "", [(dest, guest path)])
        self.size = 0
        by_hash = {}
        with tarfile.open(tar_path, "w", format=tarfile.USTAR_FORMAT) as tar:
            def add(member, data, mode):
                ti = tarfile.TarInfo(member)
                ti.size, ti.mode, ti.mtime = len(data), mode, 0
                tar.addfile(ti, io.BytesIO(data))
                self.size += len(data)

            def read(path, what):
                try:
                    if not path.is_file():
                        raise ToolError(f"{what}: {path} is not a file")
                    return path.read_bytes()
                except OSError as e:
                    raise ToolError(f"{what}: {path}: {e}")

            for j in jobs:
                code = read(j.file, f"program {j.name!r}")
                digest = hashlib.sha256(code).hexdigest()[:16]
                if digest not in by_hash:
                    by_hash[digest] = f"p/{digest}"
                    add(by_hash[digest], code, 0o755)
                stdin = ""
                if j.stdin is not None:
                    stdin = f"i/{j.index}"
                    add(stdin, read(j.stdin, f"standard input of {j.name!r}"), 0o644)
                extra = []
                for n, (dest, src) in enumerate(sorted(j.files.items())):
                    member = f"f/{j.index}/{n}"
                    add(member, read(src, f"file {dest!r} of {j.name!r}"), 0o644)
                    extra.append((dest, f"{GUEST_DIR}/{member}"))
                self.paths[j.index] = (f"{GUEST_DIR}/{by_hash[digest]}", f"{GUEST_DIR}/{stdin}" if stdin else "", extra)


def pack_manifest(jobs, payload, scale=1.0):
    """The manifest the guest's runstatus reads (user/bin/kappabox/runstatus.c): NUL-terminated strings. The time limits are multiplied by scale (TOUHOU_TIMEOUT_SCALE)."""
    out = bytearray()

    def put(v):
        b = str(v).encode("utf-8")
        assert b"\0" not in b
        out.extend(b + b"\0")

    put("RSM1")
    put(len(jobs))
    for j in jobs:
        path, stdin, extra = payload.paths[j.index]
        put(j.index)
        put(max(1, int(round(j.timeout * scale * 1000))))
        put(stdin)
        put(len(extra))
        for dest, src in extra:
            put(dest)
            put(src)
        put(path)
        put(1 + len(j.args))
        put(j.argv0 if j.argv0 is not None else path)
        for a in j.args:
            put(a)
    return bytes(out)


# ---------------------------------------------------------------- the records the guest prints

_RECORD_TEXT = r"\n@@RS (\d+) (-|\d+) (-|\d+) (\S+) (\d+) (\d+) (\S*{p})(?: (\d+) (\S*{p}))?\n|\n@@RS-DONE (\d+)\n"
try:
    # Possessive: a record that is still arriving (hundreds of kilobytes of base64 without its newline yet) is scanned once per poll instead of once per character it could give back.
    RECORD = re.compile(_RECORD_TEXT.replace("{p}", "+"))
except re.error:                                # Python before 3.11: the same pattern, slower on a long unfinished line
    RECORD = re.compile(_RECORD_TEXT.replace("{p}", ""))
DONE_GROUP = 10                                 # the group of RECORD that holds the count of an @@RS-DONE line


def _payload(b64, length):
    """The bytes of one payload of a record, and a complaint if it does not hold together."""
    try:
        raw = base64.b64decode(b64, validate=True) if b64 else b""
    except (binascii.Error, ValueError):
        return b"", "the record of this program was damaged on the way to the host"
    if len(raw) != int(length):
        return raw, "the record of this program was damaged on the way to the host (its length does not match)"
    return raw, None


def _put_text(rec, key, raw):
    """Stores raw as text under key; the exact bytes go under key_b64 when decoding them lost something."""
    text = raw.decode("utf-8", "replace")
    if text.encode("utf-8") != raw:
        rec[key + "_b64"] = base64.b64encode(raw).decode("ascii")
    rec[key] = text


def make_record(job, groups, noise):
    """One @@RS line (its groups) -> the JSON object of the result. A line with a second payload is a split one: standard output and standard error apart."""
    _, ex, sg, flags, ms, length, b64, elength, eb64 = groups[:9]
    split = elength is not None
    raw, bad = _payload(b64, length)
    eraw, ebad = _payload(eb64, elength) if split else (b"", None)
    rec = {"name": job.name}
    errors = [e for e in (bad, ebad) if e]
    if "X" in flags:                            # it could not be started: the "output" is the reason
        errors, raw, eraw = [raw.decode("utf-8", "replace")], b"", b""
    if split:
        _put_text(rec, "stdout", raw)
        _put_text(rec, "stderr", eraw)
    else:
        _put_text(rec, "output", raw)
    rec.update({"exit": None if ex == "-" else int(ex), "signal": None if sg == "-" else int(sg), "timed_out": "T" in flags})
    if errors:
        rec["error"] = errors[0]
    if "C" in flags:
        rec["truncated"] = True
    rec["ms"] = int(ms)
    if noise:
        rec["kernel"] = noise
    return rec


def error_record(job, message, kernel=(), split=False):
    rec = {"name": job.name}
    rec.update({"stdout": "", "stderr": ""} if split else {"output": ""})
    rec.update({"exit": None, "signal": None, "timed_out": False, "error": message})
    if kernel:
        rec["kernel"] = list(kernel)[-20:]
    return rec


def console_lines(text, limit=20):
    """Lines the kernel (or anything else) put on the console between two records."""
    lines = [l.strip()[:300] for l in text.split("\n") if l.strip()]
    return lines[-limit:]


# ---------------------------------------------------------------- the guest

class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class Run:
    """What one invocation needs: the staged files, the web server that hands them to the guest, and the options."""

    def __init__(self, opts, jobs):
        sys.path.insert(0, str(HERE))
        sys.path.insert(0, str(ROOT / "tests"))
        import qemu
        self.qemu = qemu
        self.opts, self.jobs = opts, jobs
        self.esp_src = host_path(opts.esp) if opts.esp else qemu.BUILD / "esp"
        if not (self.esp_src / "kernel.elf").is_file():
            raise ToolError(f"{self.esp_src} holds no built system: build it with   cd {ROOT} && python tools/build.py")
        self.tag = f"rig-{os.getpid()}"
        self.stage = qemu.BUILD / self.tag
        self.serve = self.stage / "serve"
        self.srv = None

    def __enter__(self):
        for old in self.qemu.BUILD.glob("rig-*"):                # what an earlier run left behind (killed tools do not clean up): rig-PID directories, days old
            try:
                if re.fullmatch(r"rig-\d+", old.name) and old.is_dir() and time.time() - old.stat().st_mtime > 24 * 3600:
                    shutil.rmtree(old, ignore_errors=True)
            except OSError:
                pass
        try:
            self.serve.mkdir(parents=True, exist_ok=True)
            self.payload = Payload(self.jobs, self.serve / "rig.tar")
            self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(_Quiet, directory=str(self.serve)))
            self.port = self.srv.server_address[1]
            threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        except BaseException:
            self._cleanup()
            raise
        return self

    def __exit__(self, *exc):
        self._cleanup()

    def _cleanup(self):
        if self.srv:
            self.srv.shutdown()
            self.srv.server_close()
            self.srv = None
        if not self.opts.keep:
            shutil.rmtree(self.stage, ignore_errors=True)
            for f in self.qemu.BUILD.glob(f"{self.tag}-*"):
                try:
                    f.unlink()
                except OSError:
                    pass
        else:
            print(f"run-in-guest: kept {self.stage} and build/{self.tag}-*", file=sys.stderr)

    def say(self, text):
        if self.opts.verbose:
            print(f"run-in-guest: {text}", file=sys.stderr, flush=True)

    # one guest: boots, takes the files, runs every pending program, calls emit(job, record) for each; raises GuestFailed when the guest does not get through them
    def run_guest(self, pending, attempt, emit):
        q = self.qemu
        o = self.opts
        name = f"{self.tag}-{attempt}"
        esp = self.stage / f"esp{attempt}"
        shutil.copytree(self.esp_src, esp)
        manifest = f"manifest.{attempt}"
        (self.serve / manifest).write_bytes(pack_manifest(pending, self.payload, q.scaled(1)))
        state = {"started": False}                  # the batch command is running: the first program without a result is the one that fails
        done = set()
        words = ("text " + o.cmdline).strip()
        self.say(f"starting guest {attempt + 1} for {len(pending)} program(s)")
        vm = None
        try:
            with q.Qemu(cmdline=words, name=name, esp=esp, net=True, mem=o.mem) as vm:
                self._drive(vm, pending, manifest, state, done, emit)
        except (TimeoutError, RuntimeError) as e:
            tail = console_lines(vm.serial[-3000:]) if vm is not None else []
            reason = "the guest stopped answering" if isinstance(e, TimeoutError) else "the guest went away: " + str(e).split("\n")[0][:200]
            raise GuestFailed(reason, state["started"], tail)
        finally:
            shutil.rmtree(esp, ignore_errors=True)
        return done

    def _drive(self, vm, pending, manifest, state, done, emit):
        import test_shell
        q = self.qemu
        o = self.opts
        by_index = {j.index: j for j in pending}
        vm.expect(test_shell.PROMPT, timeout=o.boot_timeout)
        out, rc = test_shell.run(vm, "which runstatus")
        if rc != 0:
            raise ToolError(f"this system has no 'runstatus' applet (it is newer than the image): rebuild it with   cd {ROOT} && python tools/build.py")
        out, rc = test_shell.run(vm, f"mkdir -p {GUEST_DIR}")
        if rc != 0:
            raise GuestFailed(f"'mkdir -p {GUEST_DIR}' failed in the guest: {out.strip()[-200:]}", False)
        deadline = time.time() + q.scaled(o.boot_timeout)
        t_fetch = time.time()
        for fname, dest in (("rig.tar", "/tmp/rig.tar"), (manifest, f"{GUEST_DIR}/manifest")):
            while True:                                      # the network comes up a little after the prompt
                out, rc = test_shell.run(vm, f"fetch -q -O {dest} http://10.0.2.2:{self.port}/{fname}", timeout=120)
                if rc == 0:
                    break
                if time.time() > deadline:
                    raise GuestFailed(f"the guest could not download {fname}: {out.strip()[-200:]}", False)
                time.sleep(3)
        self.say(f"the guest has the programs after {time.time() - t_fetch:.0f} s")
        t_tar = time.time()
        out, rc = test_shell.run(vm, f"tar -xf /tmp/rig.tar -C {GUEST_DIR}", timeout=300)
        if rc != 0:
            raise GuestFailed(f"the guest could not unpack the programs: {out.strip()[-200:]}", False)
        self.say(f"unpacked in {time.time() - t_tar:.0f} s")
        sub = "".join(f" -m {d}" for d in o.mkdir)
        split = getattr(o, "split", False)
        limit = max(1, int(q.scaled(o.timeout)))
        self.say("running")
        split_flag = " -s" if split else ""
        vm.send(f"runstatus -b {GUEST_DIR}/manifest -w {GUEST_DIR}/w -t {limit} -c {o.max_output}{split_flag}{sub}\n")
        vm.expect(r"\n@@RS-START (\d+)\n", timeout=60)
        state["started"] = True
        # How long a program's record may take to arrive after the program ended: 40 s, plus the time the console needs for a full-size output (base64 is 4/3 as long, the console
        # carries about 30 KB/s, and a split record has two outputs). The margin is generous: it only decides when a silent guest is given up on.
        margin = 40 + int(o.max_output * (2 if split else 1) * 4 / 3 / 15000)
        while len(done) < len(pending):
            before = vm._pos
            nxt = next(j for j in pending if j.index not in done)
            m = vm.expect(RECORD.pattern, timeout=nxt.timeout + margin)
            if m.group(DONE_GROUP) is not None:
                break
            noise = console_lines(vm.serial[before:m.start()])
            job = by_index.get(int(m.group(1)))
            if job is None or job.index in done:
                print(f"run-in-guest: ignoring a record for unknown program {m.group(1)}", file=sys.stderr)
                continue
            done.add(job.index)
            emit(job, make_record(job, m.groups(), noise))
        if len(done) < len(pending):
            raise GuestFailed("the guest finished without reporting every program", True)
        vm.expect(r"@@RS-DONE \d+", timeout=60)


def run_all(opts, jobs, emit):
    """Runs the jobs, in as many guests as it takes. emit(record) gets one record per job, in manifest order. Returns True if no guest failed."""
    ok = True
    setup_failures = 0
    split = getattr(opts, "split", False)
    with Run(opts, jobs) as run:
        pending = list(jobs)
        boots = 0
        while pending:
            if boots >= MAX_BOOTS or setup_failures >= 2:
                why = "the guest could not be set up twice in a row" if setup_failures >= 2 else f"{MAX_BOOTS} guests in a row failed"
                for j in pending:
                    emit(error_record(j, f"not run: {why}", split=split))
                return False
            have = set()

            def got(job, rec, _have=have):
                _have.add(job.index)
                emit(rec)

            boots += 1
            try:
                run.run_guest(pending, boots - 1, got)
                return ok
            except GuestFailed as f:
                ok = False
                run.say(f"the guest failed: {f.reason}")
                remaining = [j for j in pending if j.index not in have]
                if f.started and remaining:                      # the first program without a result is the one it was running
                    emit(error_record(remaining[0], f.reason, f.kernel, split=split))
                    remaining = remaining[1:]
                    setup_failures = 0
                elif not f.started:
                    setup_failures += 1
                pending = remaining
    return ok


def write_line(text):
    """One line of ASCII on stdout, ended by a plain newline on every platform (Windows Python would write CR LF through print), flushed."""
    sys.stdout.buffer.write(text.encode("ascii") + b"\n")
    sys.stdout.buffer.flush()


def single(opts):
    """The single form: one program, its output on stdout, its status as ours."""
    prog = host_path(opts.program[0])
    stdin_file = None
    tmp = None
    try:
        if opts.stdin == "-":
            import tempfile
            data = sys.stdin.buffer.read()
            fd, tmp = tempfile.mkstemp(prefix="rig-stdin-")
            with os.fdopen(fd, "wb") as f:
                f.write(data)
            stdin_file = Path(tmp)
        elif opts.stdin:
            stdin_file = host_path(opts.stdin)
        job = Job(0, prog.name, prog, list(opts.program[1:]), opts.timeout, stdin_file, {}, None)
        for a in job.args:
            _check_str(a, "an argument")
        records = []
        ok = run_all(opts, [job], records.append)
    finally:
        if tmp:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    rec = records[0]
    if opts.json:
        write_line(json.dumps(rec, ensure_ascii=True))
        return 0 if ok else 1
    def bytes_of(key):
        return base64.b64decode(rec[key + "_b64"]) if key + "_b64" in rec else rec[key].encode("utf-8")

    if "stdout" in rec:                         # --split: the program's two streams come out as ours
        sys.stdout.buffer.write(bytes_of("stdout"))
        sys.stdout.buffer.flush()
        sys.stderr.buffer.write(bytes_of("stderr"))
        sys.stderr.buffer.flush()
    else:
        sys.stdout.buffer.write(bytes_of("output"))
        sys.stdout.buffer.flush()
    for line in rec.get("kernel", []):
        print(f"run-in-guest: console: {line}", file=sys.stderr)
    if "error" in rec:
        print(f"run-in-guest: {rec['error']}", file=sys.stderr)
        return 125 if not ok else 126
    if rec["timed_out"]:
        print(f"run-in-guest: {prog.name} ran out of time ({opts.timeout:g} s) and was killed", file=sys.stderr)
        return 124
    if rec["signal"] is not None:
        print(f"run-in-guest: {prog.name} was killed by signal {rec['signal']} ({SIGNALS.get(rec['signal'], 'unknown')})", file=sys.stderr)
        return 128 + rec["signal"]
    return rec["exit"]


def batch(opts):
    jobs = read_manifest(opts.batch, opts.timeout)

    def emit(rec):
        write_line(json.dumps(rec, ensure_ascii=True))

    return 0 if run_all(opts, jobs, emit) else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    ap = argparse.ArgumentParser(prog="run-in-guest.py", usage="run-in-guest.py [options] PROGRAM [ARGUMENT...]\n       run-in-guest.py [options] --batch MANIFEST.json",
                                 description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--batch", metavar="MANIFEST.json", help="run every program of the manifest in one boot; one JSON line each")
    ap.add_argument("--timeout", type=float, default=10.0, metavar="SECONDS", help="how long a program may run (default 10)")
    ap.add_argument("--boot-timeout", type=float, default=240.0, metavar="SECONDS", help="how long the guest may take to boot and get its network (default 240)")
    ap.add_argument("--max-output", type=int, default=256 * 1024, metavar="BYTES", help="the most output kept from one program; one that writes more is killed (default 256 KiB; the serial console carries about 30 KB/s)")
    ap.add_argument("--split", action="store_true", help="keep standard output and standard error apart: results have \"stdout\" and \"stderr\" instead of \"output\" (the order between the two is lost)")
    ap.add_argument("--mkdir", action="append", default=[], metavar="DIR", help="make DIR (relative) in every program's working directory; may be repeated")
    ap.add_argument("--stdin", metavar="FILE", help="single form: a file for the program's standard input (- is this process's standard input)")
    ap.add_argument("--json", action="store_true", help="single form: print the result as one JSON line instead of the output")
    ap.add_argument("--esp", metavar="DIR", help="the system to boot (default build/esp)")
    ap.add_argument("--mem", default="512M", help="guest memory (default 512M)")
    ap.add_argument("--cmdline", default="", metavar="WORDS", help="more words for the kernel command line")
    ap.add_argument("--keep", action="store_true", help="keep the temporary files (staged programs, QEMU and serial logs) in build/")
    ap.add_argument("--no-slot", action="store_true", help="do not take a QEMU slot (you hold one already)")
    ap.add_argument("-v", "--verbose", action="store_true", help="say what is happening on stderr")
    ap.add_argument("program", nargs=argparse.REMAINDER, help="PROGRAM [ARGUMENT...] (options go before the program)")
    opts = ap.parse_args(argv)
    if opts.batch is None and not opts.program:
        ap.error("give a PROGRAM or --batch MANIFEST.json")
    if opts.batch is not None and opts.program:
        ap.error("--batch takes no PROGRAM")
    if opts.timeout <= 0 or opts.boot_timeout <= 0 or opts.max_output <= 0:
        ap.error("the time limits and the output limit must be positive")
    for d in opts.mkdir:
        if not SUBDIR_OK.match(d) or ".." in d.split("/"):
            ap.error(f"--mkdir {d!r}: use a relative name made of letters, digits, '_', '.' and '-'")
    if not opts.no_slot and not os.environ.get(SLOT_ENV):
        cmd = [sys.executable, str(HERE / "qemu-slot.py"), "--", sys.executable, str(Path(__file__).resolve()), "--no-slot", *argv]
        return subprocess.call(cmd)
    try:
        return batch(opts) if opts.batch is not None else single(opts)
    except ToolError as e:
        print(f"run-in-guest: {e}", file=sys.stderr)
        return 125
    except KeyboardInterrupt:
        print("run-in-guest: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
