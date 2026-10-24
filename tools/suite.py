#!/usr/bin/env python3
"""Runs the QEMU test suite split into groups, each group in its own git worktree with its own build, in parallel.

  python tools/suite.py                         the seven groups of docs/CLOUD_AGENT_BRIEF.md (plus "rest", see below)
  python tools/suite.py --groups usb,storage    only these of the named groups
  python tools/suite.py --jobs 4                four groups balanced by the durations recorded by earlier runs
  python tools/suite.py --group usb_ --group storage,~4k    groups of your own (filters as for tests/run_all.py)
  python tools/suite.py --only selftest,utest --jobs 2      any of the above, limited to the tests these filters select
  python tools/suite.py --print-groups          show which tests each group would run, run nothing

Every group gets a fresh `git worktree` of HEAD (uncommitted changes are NOT in it) in a temporary folder, builds there
once (tools/build.py) and runs tests/run_all.py --retry-failed on its tests; two groups never share a checkout (they
would fight over build/esp/cmdline.txt). Logs go to build/suite/<date-time>/: <group>.log (build and test output),
<group>.json (results), <group>-files/ (serial and QEMU logs and screenshots of a group with failures) and summary.md.
The worktrees are removed at the end, also after Ctrl-C. Exit status 1 when a test failed (flaky ones and expected
failures do not count) or a group could not run.

The named groups take the tests in order: a test goes to the first group whose filters select it, and a test that no
group names (a new prefix) goes to "rest", so nothing is skipped silently. Per-test durations of every run are kept in
build/test-durations.json; --jobs uses them (unknown tests count as the median).

For CI (.github/workflows/full-suite.yml), in the current checkout, without worktrees and without building:

  python tools/suite.py --ci-plan [--groups a,b]          the matrix as JSON (groups, the long ones split in parts)
  python tools/suite.py --run-group usb --part 1/2        run one part of one group here; writes build/suite-<label>.json
                                                          and .log, and a Markdown summary to $GITHUB_STEP_SUMMARY
  python tools/suite.py --aggregate DIR [--expect-plan J]  combine the .json files under DIR into one summary; with the
                                                          plan, a group that left no results counts as failed
"""
import argparse
import datetime
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DURATIONS = BUILD / "test-durations.json"
sys.path.insert(0, str(ROOT / "tests"))
from run_all import matches  # noqa: E402  (does not load the test cases)

# The seven groups of docs/CLOUD_AGENT_BRIEF.md ("Rules of the house"), with short names. "rest" is added at run time.
GROUPS = [
    ("usb", "usb_"),
    ("storage", "storage"),
    ("usbstor", "usbstor"),
    ("kernel", "blk,crash,selftest,utest,kshell,jobctl,hwaudit"),
    ("gui", "notebook,sketchbook"),
    ("desktop", "explorer,media,apps,desktop,snake,danmaku,games,minesweeper,lock,autologin,power,accounts,login,time,acpi,shrine"),
    ("tools", "net,audio,threads,diff,patch,bc,dc,awk,vi,shell,pnote,pager,manual,image"),
]
REST = "rest"
# How many parts each group is split into on GitHub's runners (slower than a desktop; usb_ and storage are the longest groups).
CI_PARTS = {"usb": 2, "storage": 2}


# ---------------------------------------------------------------------------------------------- choosing the tests

def list_tests(filters=""):
    """Names of the tests the filters select, in suite order (asks tests/run_all.py --list of this checkout)."""
    out = subprocess.run([sys.executable, str(ROOT / "tests" / "run_all.py"), "--list", filters],
                         capture_output=True, text=True, cwd=ROOT, check=True).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def named_groups(names, only=None):
    """[(group name, [tests])] for the named groups, every test in exactly one group; tests no group names go to "rest"."""
    taken, groups = set(), []
    for gname, filters in GROUPS:
        wants = filters.split(",")
        tests = [n for n in names if n not in taken and matches(n, wants)]
        taken.update(tests)
        groups.append((gname, tests))
    groups.append((REST, [n for n in names if n not in taken]))
    if only is not None:
        groups = [(g, [t for t in tests if t in only]) for g, tests in groups]
    return [(g, tests) for g, tests in groups if tests]


def load_durations():
    try:
        return {k: float(v) for k, v in json.loads(DURATIONS.read_text(encoding="utf-8")).items()}
    except (OSError, ValueError, AttributeError):
        return {}


def save_durations(results):
    known = load_durations()
    for r in results:
        known[r["name"]] = r["seconds"]
    BUILD.mkdir(exist_ok=True)
    DURATIONS.write_text(json.dumps(dict(sorted(known.items())), indent=1) + "\n", encoding="utf-8")


def balanced_groups(names, jobs, durations):
    """Splits the tests into `jobs` groups of about equal total time (longest first into the lightest group);
    each group keeps the suite order."""
    default = statistics.median(durations.values()) if durations else 60.0
    cost = {n: durations.get(n, default) for n in names}
    bins = [[0.0, []] for _ in range(max(1, jobs))]
    for n in sorted(names, key=lambda n: -cost[n]):
        light = min(bins, key=lambda b: b[0])
        light[0] += cost[n]
        light[1].append(n)
    order = {n: i for i, n in enumerate(names)}
    return [(f"part{i + 1}", sorted(b[1], key=order.get)) for i, b in enumerate(bins) if b[1]]


def custom_groups(specs, names):
    groups, taken = [], set()
    for i, spec in enumerate(specs):
        tests = [n for n in names if n not in taken and matches(n, [w for w in spec.split(",") if w])]
        taken.update(tests)
        groups.append((f"group{i + 1}", tests))
    return [(g, t) for g, t in groups if t]


def exact_filter(tests):
    return ",".join("=" + t for t in tests)


def part_of(tests, part):
    """part "K/N": every N-th test starting at the K-th, so long and short tests of a group spread over the parts."""
    k, n = (int(x) for x in part.split("/"))
    if not 1 <= k <= n:
        raise SystemExit(f"--part {part}: K must be between 1 and N")
    return tests[k - 1::n]


# ---------------------------------------------------------------------------------------------- results and summaries

def counts(results):
    c = {s: 0 for s in ("PASS", "FAIL", "FLAKY", "XFAIL", "XPASS")}
    for r in results:
        c[r["status"]] = c.get(r["status"], 0) + 1
    return c


def fmt_secs(s):
    s = int(round(s))
    return f"{s // 60}m{s % 60:02d}s"


def group_markdown(label, data):
    """The Markdown summary of one group (for $GITHUB_STEP_SUMMARY)."""
    if data is None or "results" not in data:
        return f"### {label}: no results\n\nThe group did not finish (build failure, crash or time limit); see its log.\n"
    res = data["results"]
    c = counts(res)
    state = "failed" if data.get("exit") else "passed"
    out = [f"### {label}: {state}", "",
           f"{len(res)} tests: {c['PASS']} passed, {c['FAIL']} failed, {c['FLAKY']} flaky, {c['XFAIL']} expected failures, "
           f"{c['XPASS']} unexpected passes; {fmt_secs(sum(r['seconds'] + r.get('retry_seconds', 0) for r in res))}", ""]
    odd = [r for r in res if r["status"] != "PASS"]
    if odd:
        out += ["| Test | Result | Seconds | Note |", "|------|--------|---------|------|"]
        for r in odd:
            note = {"FAIL": "failed, and again alone" if data.get("retry_failed") else "failed",
                    "FLAKY": "failed in the run, passed alone", "XFAIL": "expected: " + r.get("reason", ""),
                    "XPASS": "passed although expected to fail: " + r.get("reason", "")}[r["status"]]
            if r["status"] in ("FAIL", "FLAKY") and r.get("error"):
                note += ": `" + r["error"].strip().splitlines()[0][:160].replace("|", "\\|").replace("`", "'") + "`"
            out.append(f"| `{r['name']}` | {r['status']} | {r['seconds']:.1f} | {note} |")
        out.append("")
    out += ["<details><summary>All tests of the group</summary>", "", "| Test | Result | Seconds |", "|------|--------|---------|"]
    out += [f"| `{r['name']}` | {r['status']} | {r['seconds']:.1f} |" for r in res]
    out += ["", "</details>", ""]
    return "\n".join(out)


def aggregate_markdown(entries):
    """entries: [(label, data or None)] -> (markdown, failed?)."""
    rows, bad, failed_names, flaky_names, xfail_names, xpass_names = [], False, [], [], [], []
    total = counts([])
    total_tests = total_secs = 0
    for label, data in entries:
        if data is None or "results" not in data:
            rows.append(f"| {label} | did not finish | | | | | | |")
            bad = True
            continue
        res = data["results"]
        c = counts(res)
        secs = sum(r["seconds"] + r.get("retry_seconds", 0) for r in res)
        for k in total:
            total[k] += c[k]
        total_tests += len(res)
        total_secs += secs
        bad |= bool(data.get("exit"))
        failed_names += [f"`{r['name']}` ({label})" for r in res if r["status"] == "FAIL"]
        flaky_names += [f"`{r['name']}` ({label})" for r in res if r["status"] == "FLAKY"]
        xfail_names += [f"`{r['name']}` ({r.get('reason', '')})" for r in res if r["status"] == "XFAIL"]
        xpass_names += [f"`{r['name']}` ({r.get('reason', '')})" for r in res if r["status"] == "XPASS"]
        rows.append(f"| {label} | {'failed' if data.get('exit') else 'passed'} | {len(res)} | {c['PASS']} | {c['FAIL']} | "
                    f"{c['FLAKY']} | {c['XFAIL']}/{c['XPASS']} | {fmt_secs(secs)} |")
    out = [f"## Whole suite: {'FAILED' if bad else 'passed'}", "",
           f"{total_tests} tests: {total['PASS']} passed, {total['FAIL']} failed, {total['FLAKY']} flaky, "
           f"{total['XFAIL']} expected failures, {total['XPASS']} unexpected passes; {fmt_secs(total_secs)} of test time", "",
           "| Group | State | Tests | Passed | Failed | Flaky | XFail/XPass | Time |",
           "|-------|-------|-------|--------|--------|-------|-------------|------|", *rows, ""]
    for title, names in (("Failed (also when rerun alone)", failed_names), ("Flaky (failed in the run, passed alone)", flaky_names),
                         ("Expected failures", xfail_names), ("Unexpected passes (narrow the @xfail condition)", xpass_names)):
        if names:
            out += [f"**{title}:** " + ", ".join(names), ""]
    return "\n".join(out), bad


def write_step_summary(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text + "\n")


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------------------------- CI mode

def check_group_names(want):
    unknown = [w for w in want if w not in [g for g, _ in GROUPS] + [REST]]
    if unknown:
        raise SystemExit(f"unknown group(s) {', '.join(unknown)}; the groups are {', '.join(g for g, _ in GROUPS)}, {REST}")


def ci_plan(only_groups=None):
    """The matrix of .github/workflows/full-suite.yml: one entry per group part that has tests."""
    entries = []
    for gname, tests in named_groups(list_tests()):
        if only_groups and gname not in only_groups:
            continue
        n = CI_PARTS.get(gname, 1)
        for k in range(1, n + 1):
            if part_of(tests, f"{k}/{n}"):
                entries.append({"group": gname, "part": f"{k}/{n}", "label": gname if n == 1 else f"{gname}-{k}of{n}"})
    return entries


def run_group_here(args):
    groups = dict(named_groups(list_tests()))
    if args.run_group not in groups:
        raise SystemExit(f"no group {args.run_group!r} (have: {', '.join(groups)})")
    tests = groups[args.run_group]
    if args.only:
        only = set(list_tests(args.only))
        tests = [t for t in tests if t in only]
    label = args.run_group
    if args.part:
        tests = part_of(tests, args.part)
        k, n = args.part.split("/")
        label = args.run_group if n == "1" else f"{args.run_group}-{k}of{n}"
    BUILD.mkdir(exist_ok=True)
    results = Path(args.results or BUILD / f"suite-{label}.json")
    log = BUILD / f"suite-{label}.log"
    print(f"group {label}: {len(tests)} tests: {' '.join(tests)}", flush=True)
    cmd = [sys.executable, "-u", str(ROOT / "tests" / "run_all.py"), "--json", str(results)]
    if not args.no_retry:
        cmd.append("--retry-failed")
    if results.exists():
        results.unlink()
    with open(log, "w", encoding="utf-8") as f:            # the output goes to the console (the CI log) and to the log file
        proc = subprocess.Popen(cmd + [exact_filter(tests)], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace")
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            f.write(line)
        rc = proc.wait()
    data = read_json(results)
    if data is not None:
        data.update(group=args.run_group, part=args.part or "1/1", label=label)
        results.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    write_step_summary(group_markdown(label, data))
    return rc if data is not None else 1


def aggregate(args):
    found = {}
    for p in sorted(Path(args.aggregate).rglob("*.json")):
        data = read_json(p)
        if isinstance(data, dict) and "label" in data:
            found[data["label"]] = data
    if args.expect_plan:
        labels = [e["label"] for e in json.loads(args.expect_plan)["include"]]
    else:
        labels = sorted(found)
    labels += [lb for lb in sorted(found) if lb not in labels]
    md, bad = aggregate_markdown([(lb, found.get(lb)) for lb in labels])
    print(md)
    write_step_summary(md)
    return 1 if bad else 0


# ---------------------------------------------------------------------------------------------- local mode: worktrees

def git(*args, cwd=ROOT, check=True):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=check)


class Group:
    def __init__(self, name, tests, workroot, logdir, retry):
        self.name, self.tests, self.retry = name, tests, retry
        self.worktree = workroot / f"wt-{name}"
        self.log = logdir / f"{name}.log"
        self.results = logdir / f"{name}.json"
        self.files = logdir / f"{name}-files"
        self.proc = None
        self.rc = None
        self.stage = "waiting"
        self.started = self.ended = None

    def popen(self, cmd, logf):
        kw = {"cwd": self.worktree, "stdout": logf, "stderr": subprocess.STDOUT}
        if os.name == "nt":
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kw["start_new_session"] = True          # so that stopping it also stops the QEMU it started
        self.proc = subprocess.Popen(cmd, **kw)
        return self.proc.wait()

    def run(self):
        self.started = time.time()
        with open(self.log, "w", encoding="utf-8") as logf:
            logf.write(f"group {self.name}: {len(self.tests)} tests: {' '.join(self.tests)}\n\n")
            logf.flush()
            self.stage = "building"
            rc = self.popen([sys.executable, "-u", "tools/build.py"], logf)
            if rc != 0:
                logf.write(f"\nbuild failed (status {rc}): no tests ran\n")
                self.rc, self.stage, self.ended = rc or 1, "build failed", time.time()
                return
            self.stage = "testing"
            cmd = [sys.executable, "-u", "tests/run_all.py", "--json", str(self.results)]
            if self.retry:
                cmd.append("--retry-failed")
            self.rc = self.popen(cmd + [exact_filter(self.tests)], logf)
        self.stage, self.ended = "done", time.time()

    def stop(self):
        p = self.proc
        if p is None or p.poll() is not None:
            return
        try:
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
            else:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass

    def progress(self):
        try:
            text = self.log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return 0
        return len(re.findall(r"^(?:PASS|FAIL|XFAIL|XPASS) ", text, re.M))


def keep_files(g):
    """Copies the serial and QEMU logs and screenshots of a group that had failures out of its worktree before it goes."""
    src = g.worktree / "build"
    picked = [p for pat in ("*-serial.log", "*-qemu.log", "*.png") for p in src.glob(pat)]
    if picked:
        g.files.mkdir(exist_ok=True)
        for p in picked:
            shutil.copy2(p, g.files / p.name)


def remove_worktree(path):
    if path.exists():
        r = git("worktree", "remove", "--force", str(path), check=False)
        if r.returncode != 0:
            shutil.rmtree(path, ignore_errors=True)


def run_local(groups, args):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    logdir = Path(args.logdir) if args.logdir else BUILD / "suite" / stamp
    logdir.mkdir(parents=True, exist_ok=True)
    if args.workdir:
        Path(args.workdir).mkdir(parents=True, exist_ok=True)
    workroot = Path(tempfile.mkdtemp(prefix="touhou-suite-", dir=args.workdir))
    head = git("rev-parse", "HEAD").stdout.strip()
    if git("status", "--porcelain", "--untracked-files=no").stdout.strip():
        print("note: the checkout has uncommitted changes; the worktrees test the last commit, " + head[:12], flush=True)
    jobs = [Group(name, tests, workroot, logdir, not args.no_retry) for name, tests in groups]
    assert len({g.worktree for g in jobs}) == len(jobs)                # one checkout per group, always
    print(f"{len(jobs)} groups, {sum(len(g.tests) for g in jobs)} tests, commit {head[:12]}, logs in {logdir}", flush=True)
    for g in jobs:
        print(f"  {g.name:10} {len(g.tests):3} tests", flush=True)
    parallel = max(1, args.max_parallel or len(jobs))

    def on_term(signum, frame):                                        # a kill (or a cancelled CI job) cleans up like Ctrl-C
        raise KeyboardInterrupt
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGTERM, on_term)
    t0 = time.time()
    interrupted = False
    try:
        for g in jobs:
            git("worktree", "add", "--detach", str(g.worktree), head)
        pending, running = list(jobs), []
        last_report = time.time()
        while pending or running:
            while pending and len(running) < parallel:
                g = pending.pop(0)
                th = threading.Thread(target=g.run, daemon=True)
                th.start()
                running.append((g, th))
            time.sleep(1)
            for g, th in list(running):
                if not th.is_alive():
                    running.remove((g, th))
                    data = read_json(g.results)
                    c = counts(data["results"]) if data else None
                    what = (f"{c['PASS']} passed, {c['FAIL']} failed, {c['FLAKY']} flaky, {c['XFAIL']} xfail, {c['XPASS']} xpass"
                            if c else g.stage)
                    print(f"[{fmt_secs(time.time() - t0)}] {g.name} finished: {what}", flush=True)
                    if g.rc != 0:
                        keep_files(g)
                    if not args.keep_worktrees:
                        remove_worktree(g.worktree)
            if running and time.time() - last_report >= args.status_every:
                last_report = time.time()
                print(f"[{fmt_secs(time.time() - t0)}] " + ", ".join(f"{g.name} {g.stage} {g.progress()}/{len(g.tests)}"
                                                                   for g, _ in running), flush=True)
    except KeyboardInterrupt:
        interrupted = True
        print("\ninterrupted: stopping the groups", flush=True)
        for g in jobs:
            g.stop()
    finally:
        for g in jobs:
            g.stop()
            if interrupted or g.rc not in (0, None):
                if g.worktree.exists():
                    keep_files(g)
            if not args.keep_worktrees:
                remove_worktree(g.worktree)
        git("worktree", "prune", check=False)
        if not args.keep_worktrees:
            shutil.rmtree(workroot, ignore_errors=True)
    entries, all_results = [], []
    for g in jobs:
        data = read_json(g.results)
        if data is not None:
            data["label"] = g.name
            all_results += data["results"]
        entries.append((g.name, data))
    if all_results:
        save_durations(all_results)
    md, bad = aggregate_markdown(entries)
    md += f"\nCommit `{head[:12]}`, wall time {fmt_secs(time.time() - t0)}, logs in `{logdir}`.\n"
    (logdir / "summary.md").write_text(md, encoding="utf-8")
    print("\n" + md, flush=True)
    if args.keep_worktrees:
        print(f"worktrees kept in {workroot} (remove with git worktree remove)")
    return 1 if bad or interrupted else 0


# ---------------------------------------------------------------------------------------------- main

def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    how = ap.add_mutually_exclusive_group()
    how.add_argument("--groups", help="comma-separated names of the named groups to run (default: all of them)")
    how.add_argument("--jobs", type=int, help="N groups balanced by recorded test durations instead of the named groups")
    how.add_argument("--group", action="append", metavar="FILTERS", help="a group of your own (repeat for more groups)")
    ap.add_argument("--only", metavar="FILTERS", help="only the tests these run_all.py filters select")
    ap.add_argument("--max-parallel", type=int, help="run at most this many groups at once (default: all)")
    ap.add_argument("--no-retry", action="store_true", help="do not pass --retry-failed to run_all.py")
    ap.add_argument("--logdir", help="where the logs go (default build/suite/<date-time>)")
    ap.add_argument("--workdir", help="where the temporary worktrees go (default: the system's temporary folder)")
    ap.add_argument("--keep-worktrees", action="store_true", help="leave the worktrees in place (for debugging)")
    ap.add_argument("--status-every", type=float, default=60, help="seconds between progress lines (default 60)")
    ap.add_argument("--print-groups", action="store_true", help="print the groups and their tests, run nothing")
    ap.add_argument("--ci-plan", action="store_true", help="print the CI matrix as JSON")
    ap.add_argument("--run-group", metavar="NAME", help="CI: run this named group in this checkout (no worktree, no build)")
    ap.add_argument("--part", metavar="K/N", help="with --run-group: only part K of N of the group")
    ap.add_argument("--results", help="with --run-group: where the JSON results go (default build/suite-<label>.json)")
    ap.add_argument("--aggregate", metavar="DIR", help="CI: combine the group results (*.json) under DIR into one summary")
    ap.add_argument("--expect-plan", metavar="JSON", help="with --aggregate: the --ci-plan output; a planned group without results counts as failed")
    args = ap.parse_args(argv)

    if args.ci_plan:
        want = [w for w in (args.groups or "").split(",") if w]
        check_group_names(want)
        print(json.dumps({"include": ci_plan(want)}))
        return 0
    if args.run_group:
        return run_group_here(args)
    if args.aggregate:
        return aggregate(args)

    names = list_tests()
    only = set(list_tests(args.only)) if args.only else None
    pool = [n for n in names if only is None or n in only]
    if args.jobs:
        groups = balanced_groups(pool, args.jobs, load_durations())
    elif args.group:
        groups = custom_groups(args.group, pool)
    else:
        groups = named_groups(names, only)
        if args.groups:
            want = [w for w in args.groups.split(",") if w]
            check_group_names(want)
            groups = [(g, t) for g, t in groups if g in want]
    if not groups:
        raise SystemExit("no tests selected")
    if args.print_groups:
        durations = load_durations()
        for g, tests in groups:
            est = sum(durations.get(t, 0) for t in tests)
            print(f"{g} ({len(tests)} tests{', about ' + fmt_secs(est) if est else ''}): {' '.join(tests)}")
        return 0
    return run_local(groups, args)


if __name__ == "__main__":
    sys.exit(main())
