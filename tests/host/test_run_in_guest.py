#!/usr/bin/env python3
"""Host-side unit tests for tools/run-in-guest.py: the manifest, the payload and the manifest the guest reads, the result records, the restart of a guest that fails, and the
exit status of the single form. No QEMU (a fake guest stands in for the real one); the real thing is tests/case_rig.py.

    python -m unittest discover -s tests/host -p "test_run_in_guest.py" -v
"""
import base64
import contextlib
import importlib.util
import io
import json
import re
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
spec = importlib.util.spec_from_file_location("run_in_guest", REPO / "tools" / "run-in-guest.py")
rig = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rig)


def b64(b):
    return base64.b64encode(b).decode()


class Files(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def make(self, name, data=b"\x7fELFxxxx"):
        p = self.tmp / name
        p.write_bytes(data)
        return str(p)


class PathTests(unittest.TestCase):
    def test_wsl_paths_become_windows_paths_on_windows_only(self):
        self.assertEqual(rig.wsl_to_windows("/mnt/c/Users/x/y.txt", True), "C:/Users/x/y.txt")
        self.assertEqual(rig.wsl_to_windows("/mnt/d", True), "D:/")
        self.assertEqual(rig.wsl_to_windows("/mnt/c/Users/x", False), "/mnt/c/Users/x")
        self.assertEqual(rig.wsl_to_windows("build/x", True), "build/x")
        self.assertEqual(rig.wsl_to_windows("/mnt/cc/x", True), "/mnt/cc/x")                 # not a drive letter
        self.assertEqual(rig.wsl_to_windows("/home/me/x", True), "/home/me/x")


class ManifestTests(Files):
    def test_defaults_and_names(self):
        jobs = rig.parse_manifest([{"file": self.make("hello")}, {"name": "x", "file": self.make("b"), "args": ["1", "2"], "timeout": 2.5}], 10)
        self.assertEqual([j.name for j in jobs], ["hello", "x"])
        self.assertEqual([j.timeout for j in jobs], [10.0, 2.5])
        self.assertEqual(jobs[1].args, ["1", "2"])
        self.assertEqual([j.index for j in jobs], [0, 1])

    def test_object_form(self):
        jobs = rig.parse_manifest({"programs": [{"file": self.make("a")}]}, 5)
        self.assertEqual(len(jobs), 1)

    def test_rejected(self):
        bad = [
            [], {}, "x", [1], [{}], [{"file": 3}], [{"file": "x", "args": "a"}], [{"file": "x", "args": [1]}], [{"file": "x", "timeout": 0}], [{"file": "x", "timeout": True}],
            [{"file": "x", "unknown": 1}], [{"file": "x", "files": {"/abs": "y"}}], [{"file": "x", "files": {"a/../../b": "y"}}], [{"file": "x", "files": []}],
            [{"file": "x\0y"}], [{"file": "x", "args": ["a\0b"]}], [{"file": "x", "argv0": 3}],
        ]
        for b in bad:
            with self.assertRaises(rig.ToolError, msg=repr(b)):
                rig.parse_manifest(b, 10)

    def test_read_manifest_reports_bad_json(self):
        p = self.tmp / "m.json"
        p.write_text("{not json", encoding="utf-8")
        with self.assertRaises(rig.ToolError):
            rig.read_manifest(p, 10)
        with self.assertRaises(rig.ToolError):
            rig.read_manifest(self.tmp / "missing.json", 10)


def read_guest_manifest(blob):
    """The same parse as user/bin/kappabox/runstatus.c read_manifest(): NUL-terminated strings."""
    parts = blob.split(b"\0")
    assert parts.pop() == b"", "the manifest ends with a NUL"
    it = iter(parts)
    assert next(it) == b"RSM1"
    jobs = []
    for _ in range(int(next(it))):
        jid, tmo, stdin, nfiles = int(next(it)), int(next(it)), next(it).decode(), int(next(it))
        files = [(next(it).decode(), next(it).decode()) for _ in range(nfiles)]
        path = next(it).decode()
        argv = [next(it).decode() for _ in range(int(next(it)))]
        jobs.append((jid, tmo, stdin, files, path, argv))
    assert next(it, None) is None, "nothing may follow the last job"
    return jobs


class PayloadTests(Files):
    def test_tar_and_guest_manifest(self):
        a, b = self.make("a", b"\x7fELFaaaa"), self.make("b", b"\x7fELFbbbb")
        stdin, data = self.make("in.txt", b"input\n"), self.make("data", b"d")
        jobs = rig.parse_manifest([
            {"name": "one", "file": a, "args": ["x", "y z", ""], "timeout": 2.5},
            {"file": a, "stdin": stdin, "files": {"build/data.txt": data}, "argv0": "prog"},
            {"name": "three", "file": b},
        ], 10)
        tar_path = self.tmp / "rig.tar"
        payload = rig.Payload(jobs, tar_path)
        with tarfile.open(tar_path) as tar:
            members = {m.name: (m.mode, tar.extractfile(m).read()) for m in tar.getmembers()}
        # the same program is stored once; programs are executable, inputs are not
        programs = [n for n in members if n.startswith("p/")]
        self.assertEqual(len(programs), 2)
        for n in programs:
            self.assertEqual(members[n][0], 0o755)
        self.assertEqual(members["i/1"], (0o644, b"input\n"))
        self.assertEqual(members["f/1/0"], (0o644, b"d"))
        got = read_guest_manifest(rig.pack_manifest(jobs, payload, 1.0))
        prog_a, prog_b = payload.paths[0][0], payload.paths[2][0]
        self.assertEqual(prog_a, payload.paths[1][0])
        self.assertNotEqual(prog_a, prog_b)
        self.assertEqual(got[0], (0, 2500, "", [], prog_a, [prog_a, "x", "y z", ""]))          # argv[0] defaults to the guest path
        self.assertEqual(got[1], (1, 10000, "/tmp/rig/i/1", [("build/data.txt", "/tmp/rig/f/1/0")], prog_a, ["prog"]))
        self.assertEqual(got[2], (2, 10000, "", [], prog_b, [prog_b]))

    def test_time_limits_are_scaled(self):
        jobs = rig.parse_manifest([{"file": self.make("a"), "timeout": 2}], 10)
        payload = rig.Payload(jobs, self.tmp / "rig.tar")
        self.assertEqual(read_guest_manifest(rig.pack_manifest(jobs, payload, 3.0))[0][1], 6000)

    def test_missing_files_are_reported(self):
        for entry in ({"file": str(self.tmp / "nope")}, {"file": self.make("a"), "stdin": str(self.tmp / "nope")}, {"file": self.make("b"), "files": {"x": str(self.tmp / "nope")}}):
            jobs = rig.parse_manifest([entry], 10)
            with self.assertRaises(rig.ToolError, msg=repr(entry)):
                rig.Payload(jobs, self.tmp / "rig.tar")


class RecordTests(Files):
    def job(self):
        return rig.Job(0, "p", Path("p"), [], 10.0, None, {}, None)

    def groups(self, ex="0", sg="-", flags="-", ms="5", length=None, raw=b"", err=None, elength=None):
        """The groups of one @@RS line: a merged record, or a split one when err (the standard error bytes) is given"""
        tail = (None, None) if err is None else (str(len(err) if elength is None else elength), b64(err))
        return ("0", ex, sg, flags, ms, str(len(raw) if length is None else length), b64(raw)) + tail

    def test_exit_and_signal_are_kept_apart(self):
        r = rig.make_record(self.job(), self.groups(ex="136", raw=b"hi\n"), [])
        self.assertEqual((r["exit"], r["signal"], r["timed_out"], r["output"]), (136, None, False, "hi\n"))      # exits with 136: not a signal
        r = rig.make_record(self.job(), self.groups(ex="-", sg="8"), [])
        self.assertEqual((r["exit"], r["signal"]), (None, 8))
        self.assertEqual(list(r)[:5], ["name", "output", "exit", "signal", "timed_out"])

    def test_flags(self):
        r = rig.make_record(self.job(), self.groups(ex="-", sg="9", flags="T"), [])
        self.assertTrue(r["timed_out"])
        self.assertNotIn("truncated", r)
        r = rig.make_record(self.job(), self.groups(ex="-", sg="9", flags="C", raw=b"x"), [])
        self.assertTrue(r["truncated"])
        self.assertFalse(r["timed_out"])
        r = rig.make_record(self.job(), self.groups(ex="-", sg="-", flags="X", raw=b"cannot run 'p': Exec format error"), [])
        self.assertEqual(r["error"], "cannot run 'p': Exec format error")
        self.assertEqual(r["output"], "")

    def test_output_that_is_not_utf8_comes_back_exactly(self):
        raw = b"ok \xff\xfe\x00 end"
        r = rig.make_record(self.job(), self.groups(raw=raw), [])
        self.assertEqual(base64.b64decode(r["output_b64"]), raw)
        self.assertIn("�", r["output"])
        self.assertNotIn("output_b64", rig.make_record(self.job(), self.groups(raw="grüße".encode()), []))

    def test_damaged_records(self):
        r = rig.make_record(self.job(), self.groups(length=99, raw=b"abc"), [])
        self.assertIn("damaged", r["error"])
        r = rig.make_record(self.job(), ("0", "0", "-", "-", "1", "3", "!!!", None, None), [])
        self.assertIn("damaged", r["error"])
        r = rig.make_record(self.job(), self.groups(raw=b"ok", err=b"abc", elength=9), [])
        self.assertIn("damaged", r["error"])

    def test_split_records_keep_the_two_streams_apart(self):
        r = rig.make_record(self.job(), self.groups(raw=b"to stdout\n", err=b"to stderr\n"), [])
        self.assertEqual(list(r)[:6], ["name", "stdout", "stderr", "exit", "signal", "timed_out"])
        self.assertEqual((r["stdout"], r["stderr"]), ("to stdout\n", "to stderr\n"))
        self.assertNotIn("output", r)
        r = rig.make_record(self.job(), self.groups(raw=b"", err=b"\xff"), [])
        self.assertEqual((r["stdout"], r["stderr_b64"]), ("", b64(b"\xff")))
        self.assertNotIn("stdout_b64", r)
        r = rig.make_record(self.job(), self.groups(ex="-", flags="X", raw=b"cannot run 'p': Exec format error", err=b""), [])
        self.assertEqual((r["stdout"], r["stderr"], r["error"]), ("", "", "cannot run 'p': Exec format error"))

    def test_error_records_follow_the_mode(self):
        job = self.job()
        self.assertEqual(rig.error_record(job, "boom")["output"], "")
        r = rig.error_record(job, "boom", split=True)
        self.assertEqual((r["stdout"], r["stderr"]), ("", ""))
        self.assertNotIn("output", r)

    def test_the_records_the_guest_prints_are_found(self):
        text = ("noise\n[Eiki] something\n\n@@RS 4 - 8 - 3 0 \n\n@@RS 5 0 - - 1 4 YWJjZA==\nmore\n\n@@RS 6 0 - - 2 1 YQ== 1 Yg==\n\n@@RS 7 0 - - 2 0  0 \n\n@@RS-DONE 8\n")
        found = [m.groups() for m in rig.RECORD.finditer("\n" + text)]          # as the guest prints them: a newline first, then the record
        self.assertEqual(found[0][:9], ("4", "-", "8", "-", "3", "0", "", None, None))
        self.assertEqual(found[1][:9], ("5", "0", "-", "-", "1", "4", "YWJjZA==", None, None))
        self.assertEqual(found[2][:9], ("6", "0", "-", "-", "2", "1", "YQ==", "1", "Yg=="))              # a split record: stdout, then stderr
        self.assertEqual(found[3][:9], ("7", "0", "-", "-", "2", "0", "", "0", ""))                       # both empty
        self.assertEqual(found[4][rig.DONE_GROUP - 1], "8")

    def test_the_json_line_is_plain_ascii(self):
        r = rig.make_record(self.job(), self.groups(raw="grüße\n".encode()), ["[Eiki] ü"])
        line = json.dumps(r, ensure_ascii=True)
        line.encode("ascii")
        self.assertEqual(json.loads(line)["output"], "grüße\n")


class FakeVM:
    """A console that answers like the guest's shell (the marker protocol of tests/test_shell.py) and prints a scripted run of runstatus when the batch command is typed."""
    PROMPT = "reimu@gensokyo ~ $ "

    def __init__(self, script, has_runstatus=True):
        self.serial = self.PROMPT
        self._pos = 0
        self.script, self.has_runstatus = script, has_runstatus
        self.sent = []

    def send(self, text):
        self.sent.append(text)
        m = re.match(r"echo __BE''GIN__; (.*); echo __EN''D__\$\?\n$", text, re.S)
        if m:
            which = m.group(1).startswith("which")
            rc = 0 if (self.has_runstatus or not which) else 1
            self.serial += "\n__BEGIN__\n" + ("/bin/runstatus\n" if which and rc == 0 else "") + f"__END__{rc}\n" + self.PROMPT
        elif text.startswith("runstatus -b"):
            self.serial += text + self.script

    def expect(self, pattern, timeout=30, flags=re.S):
        m = re.compile(pattern, flags).search(self.serial, self._pos)
        if not m:
            raise TimeoutError(f"timeout waiting for /{pattern}/")
        self._pos = m.end()
        return m


class DriveTests(Files):
    """Run._drive against a scripted console: how the records are read, mapped to programs and reported."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REPO / "tools"))
        sys.path.insert(0, str(REPO / "tests"))
        try:
            import test_shell                                    # imports tools/qemu.py, which looks for QEMU: skip where there is none
            cls.PROMPT = test_shell.PROMPT
        except BaseException as e:                               # noqa: BLE001 (SystemExit too)
            raise unittest.SkipTest(f"test_shell is not importable here: {e}")

    def stub(self, **opts):
        class O:
            boot_timeout, timeout, max_output, mkdir, cmdline, mem = 5.0, 10.0, 1 << 20, [], "", "512M"
            split = False

        for k, v in opts.items():
            setattr(O, k, v)

        class Q:
            @staticmethod
            def scaled(x):
                return x

        class Stub:
            port = 1234
            qemu = Q
            opts = O
            _drive = rig.Run._drive

            def say(self, text):
                pass

        return Stub()

    def jobs(self, n=3):
        return [rig.Job(i, f"p{i}", Path(f"p{i}"), [], 10.0, None, {}, None) for i in range(n)]

    def drive(self, script, n=3, **kw):
        jobs = self.jobs(n)
        vm = FakeVM(script, **kw)
        state, done, got = {"started": False}, set(), []
        try:
            self.stub()._drive(vm, jobs, "manifest.0", state, done, lambda job, rec: got.append((job.name, rec)))
            error = None
        except BaseException as e:                               # noqa: BLE001
            error = e
        return got, state, done, error, vm

    START, DONE = "\n@@RS-START 3\n", "\n@@RS-DONE 3\n"

    def rec(self, i, ex="0", sg="-", flags="-", raw=b""):
        return f"\n@@RS {i} {ex} {sg} {flags} 5 {len(raw)} {b64(raw)}\n"

    def test_records_become_results_in_order_with_the_console_lines_that_came_before_them(self):
        script = (self.START + "[Eiki] pid 9 died of SIGFPE\n" + self.rec(0, "-", "8", raw=b"partial") + self.rec(1, "3", raw=b"hi\n") + self.rec(2) + self.DONE)
        got, state, done, error, vm = self.drive(script)
        self.assertIsNone(error)
        self.assertTrue(state["started"])
        self.assertEqual(done, {0, 1, 2})
        self.assertEqual([n for n, _ in got], ["p0", "p1", "p2"])
        r0, r1, r2 = (r for _, r in got)
        self.assertEqual((r0["exit"], r0["signal"], r0["output"], r0["kernel"]), (None, 8, "partial", ["[Eiki] pid 9 died of SIGFPE"]))
        self.assertEqual((r1["exit"], r1["signal"], r1["output"]), (3, None, "hi\n"))
        self.assertNotIn("kernel", r1)
        self.assertEqual(r2["output"], "")
        self.assertTrue(any(s.startswith("runstatus -b /tmp/rig/manifest -w /tmp/rig/w -t 10 -c 1048576") for s in vm.sent), vm.sent)

    def test_the_batch_command_carries_split_and_mkdir(self):
        script = self.START + self.rec(0) + self.DONE
        jobs, vm = self.jobs(1), None
        vm = FakeVM(script)
        self.stub(split=True, mkdir=["build", "out/x"])._drive(vm, jobs, "manifest.0", {"started": False}, set(), lambda j, r: None)
        batch = next(s for s in vm.sent if s.startswith("runstatus -b"))
        self.assertIn(" -s", batch)
        self.assertIn(" -m build -m out/x", batch)

    def test_a_record_for_an_unknown_program_is_ignored(self):
        script = self.START + self.rec(7) + self.rec(0) + self.rec(1) + self.rec(2) + self.DONE
        got, state, done, error, vm = self.drive(script)
        self.assertIsNone(error)
        self.assertEqual([n for n, _ in got], ["p0", "p1", "p2"])

    def test_silence_after_some_records_is_a_timeout_and_says_how_far_it_got(self):
        got, state, done, error, vm = self.drive(self.START + self.rec(0) + self.rec(1))
        self.assertIsInstance(error, TimeoutError)
        self.assertEqual(done, {0, 1})
        self.assertTrue(state["started"])

    def test_no_start_line_is_a_timeout_before_the_batch_began(self):
        got, state, done, error, vm = self.drive("")
        self.assertIsInstance(error, TimeoutError)
        self.assertFalse(state["started"])

    def test_done_before_every_program_reported_blames_the_batch(self):
        got, state, done, error, vm = self.drive(self.START + self.rec(0) + self.DONE)
        self.assertIsInstance(error, rig.GuestFailed)
        self.assertTrue(error.started)
        self.assertEqual(done, {0})

    def test_an_image_without_the_applet_is_reported_as_such(self):
        got, state, done, error, vm = self.drive(self.START, has_runstatus=False)
        self.assertIsInstance(error, rig.ToolError)
        self.assertIn("runstatus", str(error))


class FakeRun:
    """Stands in for Run: a script says what each guest does."""
    script = []                 # one entry per guest: list of ("ok", index) | ("fail", reason, started)

    def __init__(self, opts, jobs):
        self.jobs = jobs
        self.boots = 0

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass

    def say(self, text):
        pass

    def run_guest(self, pending, attempt, emit):
        steps = self.script[attempt]
        for step in steps:
            if step[0] == "ok":
                job = next(j for j in pending if j.index == step[1])
                emit(job, {"name": job.name, "output": "", "exit": 0, "signal": None, "timed_out": False})
            else:
                raise rig.GuestFailed(step[1], step[2], ["[Eiki] panic"])


class Opts:
    verbose = False
    timeout = 10.0


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.real = rig.Run
        rig.Run = FakeRun
        self.addCleanup(setattr, rig, "Run", self.real)
        self.jobs = [rig.Job(i, f"p{i}", Path(f"p{i}"), [], 10.0, None, {}, None) for i in range(5)]

    def run_driver(self, script):
        FakeRun.script = script
        got = []
        ok = rig.run_all(Opts(), self.jobs, got.append)
        return ok, got

    def test_everything_runs_in_one_guest(self):
        ok, got = self.run_driver([[("ok", i) for i in range(5)]])
        self.assertTrue(ok)
        self.assertEqual([r["name"] for r in got], ["p0", "p1", "p2", "p3", "p4"])

    def test_a_crash_blames_the_program_that_was_running_and_carries_on_in_a_new_guest(self):
        ok, got = self.run_driver([[("ok", 0), ("ok", 1), ("fail", "the guest stopped answering", True)], [("ok", 3), ("ok", 4)]])
        self.assertFalse(ok)
        self.assertEqual([r["name"] for r in got], ["p0", "p1", "p2", "p3", "p4"])           # still one result per program, in order
        self.assertEqual(got[2]["error"], "the guest stopped answering")
        self.assertEqual(got[2]["kernel"], ["[Eiki] panic"])
        self.assertNotIn("error", got[3])

    def test_a_guest_that_cannot_be_set_up_twice_gives_up(self):
        ok, got = self.run_driver([[("fail", "no network", False)], [("fail", "no network", False)]])
        self.assertFalse(ok)
        self.assertEqual(len(got), 5)
        self.assertTrue(all("error" in r for r in got))

    def test_a_setup_failure_is_retried_without_blaming_a_program(self):
        ok, got = self.run_driver([[("fail", "no network", False)], [("ok", i) for i in range(5)]])
        self.assertFalse(ok)                                                                  # something failed, and the tool says so ...
        self.assertEqual([r["name"] for r in got], ["p0", "p1", "p2", "p3", "p4"])
        self.assertTrue(all("error" not in r for r in got))                                  # ... but every program ran

    def test_too_many_crashes_give_up(self):
        ok, got = self.run_driver([[("fail", "boom", True)] for _ in range(rig.MAX_BOOTS)])
        self.assertFalse(ok)
        self.assertEqual(len(got), 5)
        self.assertTrue(all("error" in r for r in got))


class SingleTests(Files):
    """The exit status of the single form for each way a program can end."""

    def run_single(self, rec, ok=True, argv=("prog",), **kw):
        class O:
            program = list(argv)
            stdin = None
            json = kw.get("json", False)
            timeout = 3.0
            verbose = False

        class FakeAll:
            def __call__(self, opts, jobs, emit):
                emit(rec)
                return ok

        real = rig.run_all
        rig.run_all = FakeAll()
        out, err = io.BytesIO(), io.BytesIO()

        class Stream:                                   # stands in for sys.stdout / sys.stderr: text and bytes go to one buffer
            def __init__(self, buf):
                self.buffer = buf

            def write(self, s):
                self.buffer.write(s.encode())

            def flush(self):
                pass

        real_out, real_err = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = Stream(out), Stream(err)
        try:
            rc = rig.single(O())
        finally:
            sys.stdout, sys.stderr = real_out, real_err
            rig.run_all = real
        return rc, out.getvalue(), err.getvalue().decode()

    def rec(self, **kw):
        base = {"name": "prog", "output": "", "exit": 0, "signal": None, "timed_out": False}
        base.update(kw)
        return base

    def test_exit_status_is_the_programs(self):
        rc, out, err = self.run_single(self.rec(exit=7, output="hello\n"))
        self.assertEqual((rc, out), (7, b"hello\n"))
        self.assertEqual(err, "")

    def test_signal_gives_128_plus_n_and_a_message(self):
        rc, out, err = self.run_single(self.rec(exit=None, signal=8, output="partial"))
        self.assertEqual((rc, out), (136, b"partial"))
        self.assertIn("SIGFPE", err)

    def test_exit_136_is_not_a_signal(self):
        rc, out, err = self.run_single(self.rec(exit=136))
        self.assertEqual((rc, err), (136, ""))

    def test_timeout_is_124(self):
        rc, out, err = self.run_single(self.rec(exit=None, signal=9, timed_out=True))
        self.assertEqual(rc, 124)
        self.assertIn("ran out of time", err)

    def test_could_not_start_is_126_and_a_failed_guest_is_125(self):
        rc, out, err = self.run_single(self.rec(exit=None, error="cannot run 'x': Exec format error"))
        self.assertEqual(rc, 126)
        rc, out, err = self.run_single(self.rec(exit=None, error="the guest stopped answering"), ok=False)
        self.assertEqual(rc, 125)

    def test_output_bytes_are_exact(self):
        rc, out, err = self.run_single(self.rec(output="�", output_b64=b64(b"\xff\x00a")))
        self.assertEqual(out, b"\xff\x00a")

    def test_split_streams_come_out_as_ours(self):
        rec = {"name": "prog", "stdout": "to stdout\n", "stderr": "to stderr\n", "exit": 101, "signal": None, "timed_out": False}
        rc, out, err = self.run_single(rec)
        self.assertEqual((rc, out, err), (101, b"to stdout\n", "to stderr\n"))

    def test_json(self):
        rc, out, err = self.run_single(self.rec(exit=3), json=True)
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["exit"], 3)


if __name__ == "__main__":
    unittest.main()
