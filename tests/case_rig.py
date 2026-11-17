"""run-in-guest: the guest's runstatus applet and the host tool tools/run-in-guest.py that runs programs in a guest and tells how each one ended (docs/RUN_IN_GUEST.md).

The programs are tiny native executables made in Python (tests/rig_elf.py): they exit with a chosen status, write to standard output and standard error, divide by zero, read
address 0, execute an undefined instruction, loop for ever, write for ever, copy their input, print their arguments. The pure-Python parts of the tool (manifest, tar, records,
restarting a guest that fails, the exit status of the single form) have host tests in tests/host/test_run_in_guest.py.

    python tools/qemu-slot.py -- python tests/run_all.py rig_
"""
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rig_elf
import test_shell
from harness import test, Qemu, BUILD, scaled

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "run-in-guest.py"
KAPPABOX = BUILD / "user" / "bin" / "kappabox"


def make_programs(name):
    """Writes every program of rig_elf into build/<name>/ and returns {program name: path}."""
    d = BUILD / name
    d.mkdir(parents=True, exist_ok=True)
    paths = {}
    for n, blob in rig_elf.PROGRAMS.items():
        (d / n).write_bytes(blob)
        paths[n] = str(d / n)
    return d, paths


def run_tool(args, minutes=15):
    """Runs the tool (this test holds the QEMU slot already). Returns (status, stdout bytes, stderr text)."""
    r = subprocess.run([sys.executable, str(TOOL), "--no-slot", *args], capture_output=True, timeout=scaled(minutes * 60))
    return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")


@test
def rig_runstatus_applet():
    """runstatus by hand: exit code and signal kept apart, a time limit, an output file, a program that cannot be started, and nothing of the program's group left behind"""
    with Qemu(cmdline="text", name="t-rig-applet") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)

        def sh(cmd, timeout=60):
            out, rc = test_shell.run(vm, cmd, timeout=timeout)
            return out.replace("\r", ""), rc

        out, rc = sh("runstatus /bin/false")
        assert rc == 1 and "exit 1" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus /bin/true")
        assert rc == 0 and "exit 0" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus -t 1 sleep 30")
        assert rc == 124 and "timed out" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus -t 0.5 sleep 30")
        assert rc == 124, f"a fractional limit: {rc} {out!r}"
        out, rc = sh("runstatus /bin/gapsh -c 'kill -8 $$'")
        assert rc == 136 and "killed by signal 8 (SIGFPE)" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus /bin/gapsh -c 'exit 136'")
        assert rc == 136 and "exit 136" in out and "signal" not in out, f"an exit with 136 must not look like a signal: {rc} {out!r}"
        out, rc = sh("runstatus /no/such/program")
        assert rc == 127 and "cannot run" in out and "No such file" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus /etc/passwd")
        assert rc == 126 and "cannot run" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus")
        assert rc == 125 and "usage" in out, f"{rc} {out!r}"
        out, rc = sh("runstatus -b /no/such/manifest")
        assert rc == 125, f"{rc} {out!r}"

        out, rc = sh("runstatus -o /tmp/rs.out /bin/gapsh -c 'echo one; echo two >&2; echo three'; cat /tmp/rs.out")
        assert "one\ntwo\nthree\n" in out, f"standard output and standard error go to one file, in order: {out!r}"
        out, rc = sh("echo some input > /tmp/rs.in; runstatus -i /tmp/rs.in -o /tmp/rs.out /bin/cat; cat /tmp/rs.out")
        assert "some input" in out, f"-i feeds the program's standard input: {out!r}"
        out, rc = sh("mkdir -p /tmp/rs.dir; runstatus -C /tmp/rs.dir -o /tmp/rs.out /bin/gapsh -c pwd; cat /tmp/rs.out")
        assert "/tmp/rs.dir" in out, f"-C sets the working directory: {out!r}"
        out, rc = sh("runstatus -o /tmp/s.out -e /tmp/s.err /bin/gapsh -c 'echo one; echo two >&2; echo three'; cat /tmp/s.out; echo ---; cat /tmp/s.err")
        assert "one\nthree\n---\ntwo\n" in out, f"-e gives standard error a file of its own: {out!r}"
        out, rc = sh("runstatus -e /tmp/x /bin/true")
        assert rc == 125 and "-e needs -o" in out, f"{rc} {out!r}"

        # a helper the program started and left running is killed with its group
        out, rc = sh("runstatus /bin/gapsh -c 'sleep 100 & echo started'")
        assert "started" in out and rc == 0, f"{rc} {out!r}"
        out, rc = sh("ps | grep -c '[s]leep'")                          # ps shows the name of a process, not its arguments
        assert out.strip().endswith("0"), f"the program's helper is still running:\n{out}"


@test
def rig_batch_tool():
    """tools/run-in-guest.py --batch: many programs in one boot; output, exit code, signal and time-out come back apart, in order, as JSON lines"""
    d, p = make_programs("rig-test-programs")
    (d / "input.txt").write_bytes(b"first line\nsecond line\n")
    (d / "data.txt").write_bytes(b"from the host\n")
    (d / "not-an-elf").write_bytes(b"this is text, not a program\n")
    entries = [
        {"name": "exit7", "file": p["exit_7"]},
        {"name": "exit0", "file": p["exit_0"]},
        {"name": "exit200", "file": p["exit_200"]},
        {"name": "argc", "file": p["exit_argc"], "args": ["a", "b"]},
        {"name": "streams", "file": p["three_lines"]},
        {"name": "fpe", "file": p["divide_by_zero"]},
        {"name": "segv", "file": p["null_read"]},
        {"name": "ill", "file": p["undefined_instruction"]},
        {"name": "spin", "file": p["spin"], "timeout": 2},
        {"name": "flood", "file": p["flood"]},
        {"name": "cat", "file": p["cat"], "stdin": str(d / "input.txt")},
        {"name": "empty-stdin", "file": p["cat"]},
        {"name": "args", "file": p["argv_lines"], "args": ["a b", "", "grüße", "--flag", "x'y\"z"]},
        {"name": "argv0", "file": p["argv_lines"], "argv0": "custom-name", "args": ["only"]},
        {"name": "not-elf", "file": str(d / "not-an-elf")},
        {"name": "after", "file": p["exit_7"]},
    ]
    if KAPPABOX.is_file():
        entries.append({"name": "files", "file": str(KAPPABOX), "argv0": "cat", "args": ["build/data.txt"], "files": {"build/data.txt": str(d / "data.txt")}})
        entries.append({"name": "mkdir", "file": str(KAPPABOX), "argv0": "ls", "args": ["-d", "build"]})
    (d / "manifest.json").write_text(json.dumps(entries), encoding="utf-8")
    rc, out, err = run_tool(["--batch", str(d / "manifest.json"), "--mkdir", "build", "--max-output", "65536", "-v"])
    assert rc == 0, f"the tool failed ({rc}):\n{err}"
    got = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    assert [r["name"] for r in got] == [e["name"] for e in entries], f"one result per program, in order: {[r['name'] for r in got]}"
    by = {r["name"]: r for r in got}

    def ended(name, exit_=None, signal=None, timed_out=False):
        r = by[name]
        assert (r["exit"], r["signal"], r["timed_out"]) == (exit_, signal, timed_out), f"{name}: {r}"
        return r

    assert ended("exit7", 7)["output"] == ""
    ended("exit0", 0)
    ended("exit200", 200)
    ended("argc", 3)
    assert ended("streams", 0)["output"] == "out1\nerr\nout2\n", by["streams"]
    r = ended("fpe", signal=8)
    assert r["output"] == "before the fault", r
    assert ended("segv", signal=11)["output"] == "partial", by["segv"]
    ended("ill", signal=4)
    r = ended("spin", signal=9, timed_out=True)
    assert r["output"] == "started\n" and 1500 <= r["ms"] < 20000, r
    r = ended("flood", signal=9)
    assert r.get("truncated") and "timed_out" in r and not r["timed_out"], f"output cap: {r}"
    assert ended("cat", 0)["output"] == "first line\nsecond line\n", by["cat"]
    assert ended("empty-stdin", 0)["output"] == "", by["empty-stdin"]
    assert ended("args", 0)["output"] == "a b\n\ngrüße\n--flag\nx'y\"z\n", by["args"]
    assert ended("argv0", 0)["output"] == "only\n", by["argv0"]
    r = by["not-elf"]
    assert r["exit"] is None and r["signal"] is None and "cannot run" in r["error"], r
    ended("after", 7)
    if "files" in by:
        assert ended("files", 0)["output"] == "from the host\n", by["files"]
        assert ended("mkdir", 0)["output"] == "build\n", by["mkdir"]


@test
def rig_big_bss():
    """a program whose .bss (2 GiB) is larger than the guest's RAM (512 MB) loads and runs: the part of a segment beyond its file data is demand-zero, and a system call can read it untouched"""
    d, p = make_programs("rig-test-bss")
    entries = [{"name": "big", "file": p["big_bss"]}, {"name": "after", "file": p["exit_7"]}]
    (d / "manifest.json").write_text(json.dumps(entries), encoding="utf-8")
    rc, out, err = run_tool(["--batch", str(d / "manifest.json")])
    assert rc == 0, f"the tool failed ({rc}):\n{err}"
    got = {r["name"]: r for r in (json.loads(l) for l in out.decode("utf-8").splitlines())}
    r = got["big"]
    assert "error" not in r, f"the loader allocated the whole .bss: {r}"
    assert (r["exit"], r["signal"]) == (0, None), r
    assert r["output"] == "\0" * 8 + "A", f"untouched .bss reads as zeros, a touched byte keeps its value: {r['output']!r}"
    assert (got["after"]["exit"], got["after"]["signal"]) == (7, None), got["after"]


@test
def rig_split_streams():
    """--split: standard output and standard error come back apart, as the program wrote them (a runner that compares them separately needs this); input from a file, an exit code and a signal"""
    d, p = make_programs("rig-test-split")
    (d / "input.txt").write_bytes(b"typed\n")
    (d / "not-an-elf").write_bytes(b"text\n")
    entries = [
        {"name": "streams", "file": p["three_lines"]},
        {"name": "fpe", "file": p["divide_by_zero"]},
        {"name": "cat", "file": p["cat"], "stdin": str(d / "input.txt")},
        {"name": "exit7", "file": p["exit_7"]},
        {"name": "not-elf", "file": str(d / "not-an-elf")},
    ]
    (d / "manifest.json").write_text(json.dumps(entries), encoding="utf-8")
    rc, out, err = run_tool(["--batch", str(d / "manifest.json"), "--split"])
    assert rc == 0, f"the tool failed ({rc}):\n{err}"
    got = {r["name"]: r for r in (json.loads(l) for l in out.decode("utf-8").splitlines())}
    r = got["streams"]
    assert (r["stdout"], r["stderr"], r["exit"], r["signal"]) == ("out1\nout2\n", "err\n", 0, None) and "output" not in r, r
    r = got["fpe"]
    assert (r["stdout"], r["stderr"], r["exit"], r["signal"]) == ("before the fault", "", None, 8), r
    assert got["cat"]["stdout"] == "typed\n" and got["cat"]["stderr"] == "", got["cat"]
    assert (got["exit7"]["exit"], got["exit7"]["stdout"], got["exit7"]["stderr"]) == (7, "", ""), got["exit7"]
    r = got["not-elf"]
    assert "cannot run" in r["error"] and r["stdout"] == "" and r["stderr"] == "", r


@test
def rig_single_form():
    """tools/run-in-guest.py PROGRAM: the output on stdout, the program's exit status as ours (128+N after a signal); a host file can be the program's input"""
    d, p = make_programs("rig-test-single")
    rc, out, err = run_tool(["--timeout", "10", p["divide_by_zero"]])
    assert rc == 128 + 8, f"a program killed by SIGFPE: status {rc}\n{err}"
    assert out == b"before the fault", out
    assert "SIGFPE" in err, err
    (d / "input.txt").write_bytes(b"line one\nline two\n")
    rc, out, err = run_tool(["--stdin", str(d / "input.txt"), p["cat"]])
    assert (rc, out) == (0, b"line one\nline two\n"), f"{rc} {out!r}\n{err}"
