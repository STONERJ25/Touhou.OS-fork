#!/usr/bin/env python3
"""Runs the test262 conformance suite (https://github.com/tc39/test262) against Kosuzu, the JavaScript engine.

    python tests/host/test262.py                         # all of test/language, test/built-ins, test/annexB, test/harness
    python tests/host/test262.py built-ins/RegExp        # a directory or a file below test/ (several allowed)
    python tests/host/test262.py --asan language/expressions
    python tests/host/test262.py --json build/t262.json --compare build/t262-before.json

Where test262 is: --dir DIR, else the environment variable TEST262_DIR, else build/test262 of this repository, else
the lead's checkout (<checkout>\\build\\test262).  Nothing is downloaded or modified.

How it works: the metadata of every test (the YAML front matter: includes, flags, features, negative) is read here;
tests that need a feature the engine does not have yet (UNSUPPORTED_FEATURES below, matched on the `features:` list)
or that belong to a part of the suite that does not apply (intl402, staging) are skipped and counted separately.  The
rest become runs (sloppy and strict, or as the flags say) listed in a manifest for tests/host/t262tool.c, which runs
each in a fresh runtime and realm, with the harness files, $262 and print as INTERPRETING.md says.  Runs are spread
over processes; a run that crashes the tool is recorded as CRASH and the rest continue.

On Windows the script re-runs itself in WSL (the engine's host tools are built with gcc there, like the other host tests).
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
JS = REPO / "user" / "lib" / "js"
LEAD_DIR = r"<checkout>\build\test262"

# Features the engine does not have yet.  Remove an entry when the feature lands: the tests that need it start to count.
UNSUPPORTED_FEATURES = {
    # not started
    "Float16Array", "Atomics.waitAsync", "uint8array-base64", "immutable-arraybuffer",
    "Intl", "Intl-enumeration", "Temporal", "ShadowRealm", "IsHTMLDDA", "AbstractModuleSource",
    "import-attributes", "json-modules", "source-phase-imports",
    "source-phase-imports-module-source", "module-defer", "import-defer", "explicit-resource-management",
    "iterator-sequencing", "iterator-chunking", "iterator-includes", "Iterator.prototype.join", "joint-iteration", "upsert", "Math.sumPrecise",
    "Error.isError", "json-parse-with-source", "error-stack-accessor", "canonical-tz", "legacy-regexp", "regexp-v-flag-properties-of-strings",
    "nonextensible-applies-to-private", "decorators", "Symbol.dispose", "Symbol.asyncDispose", "await-dictionary",
    "Atomics.pause", "composition-event",
    # not a goal: proper tail calls (only one engine implements them)
    "tail-call-optimization",
}
# Parts of the suite that do not apply (they need ECMA-402, or are proposals in staging)
SKIPPED_DIRS = ("intl402", "staging")
SKIPPED_FLAGS = {"CanBlockIsTrue", "CanBlockIsFalse"}
DEFAULT_ROOTS = ["language", "built-ins", "annexB", "harness"]


def in_wsl():
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


def native_path(p):
    """C:\\a\\b as /mnt/c/a/b where Windows paths are not understood."""
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", p)
    if m and os.name != "nt":
        return "/mnt/" + m.group(1).lower() + "/" + m.group(2).replace("\\", "/")
    return p


def find_test262(arg):
    cands = [arg, os.environ.get("TEST262_DIR"), str(REPO / "build" / "test262"), LEAD_DIR]
    for c in cands:
        if c:
            p = Path(native_path(c))
            if (p / "harness" / "assert.js").exists() and (p / "test").is_dir():
                return p
    sys.exit("test262 not found: pass --dir or set TEST262_DIR")


# ------------------------------------------------------------------ metadata

FRONT = re.compile(r"/\*---(.*?)---\*/", re.S)


def parse_front_matter(text):
    m = FRONT.search(text)
    meta = {"includes": [], "flags": [], "features": [], "negative": None,
            "agents": "$262.agent.start" in text or "$262.agent.safeBroadcast" in text}      # needs a second agent (thread)
    if not m:
        return meta
    lines = m.group(1).split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.strip() or line[0] in " \t":
            continue
        key, _, rest = line.partition(":")
        key, rest = key.strip(), rest.strip()
        if key in ("includes", "flags", "features", "locale"):
            if rest.startswith("["):
                items = [x.strip() for x in rest.strip("[]").split(",") if x.strip()]
            elif rest:
                items = [rest]
            else:
                items = []
                while i < len(lines) and re.match(r"^\s+-\s", lines[i]):
                    items.append(lines[i].split("-", 1)[1].strip())
                    i += 1
            meta[key] = items
        elif key == "negative":
            neg = {}
            while i < len(lines) and lines[i][:1] in (" ", "\t"):
                k, _, v = lines[i].strip().partition(":")
                neg[k.strip()] = v.strip()
                i += 1
            meta["negative"] = neg
    return meta


def collect(test_root, roots):
    files = []
    for r in roots:
        p = test_root / r
        if p.is_file():
            files.append(p)
        else:
            files.extend(sorted(p.rglob("*.js")))
    return [f for f in files if "_FIXTURE" not in f.name]


def plan(test_root, roots, skip_features, no_skip):
    """-> (runs, skipped): runs are (id, file, mode, includes, flags, neg_phase, neg_type); skipped maps path -> reason"""
    runs, skipped = [], {}
    # reading 50000 small files through WSL's /mnt/c takes minutes: the metadata is cached by (mtime, size)
    cache_file = REPO / "build" / "t262-meta2.json"
    try:
        cache = json.loads(cache_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        cache = {}
    dirty = False
    for f in collect(test_root, roots):
        rel = f.relative_to(test_root).as_posix()
        if not no_skip and rel.split("/")[0] in SKIPPED_DIRS:
            skipped[rel] = "dir"
            continue
        try:
            st = f.stat()
        except OSError:
            continue
        key = [st.st_mtime_ns, st.st_size]
        hit = cache.get(rel)
        if hit and hit[0] == key:
            meta = hit[1]
        else:
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            meta = parse_front_matter(text)
            cache[rel] = [key, meta]
            dirty = True
        flags = set(meta["flags"])
        if not no_skip:
            missing = sorted(set(meta["features"]) & skip_features)
            if missing:
                skipped[rel] = "feature " + missing[0]
                continue
            if flags & SKIPPED_FLAGS:
                skipped[rel] = "flag " + sorted(flags & SKIPPED_FLAGS)[0]
                continue
            if meta.get("agents"):
                skipped[rel] = "needs a second agent"       # $262.agent.start: there are no threads (SharedArrayBuffer is single-agent)
                continue
        if "raw" in flags:
            modes = ["raw"]
        elif "module" in flags:
            modes = ["module"]
        elif "onlyStrict" in flags:
            modes = ["strict"]
        elif "noStrict" in flags:
            modes = ["sloppy"]
        else:
            modes = ["sloppy", "strict"]
        neg = meta["negative"] or {}
        for mode in modes:
            runs.append((rel + "#" + mode, str(f), mode, ",".join(meta["includes"]), ",".join(meta["flags"]),
                         neg.get("phase", ""), neg.get("type", "")))
    if dirty:
        try:
            cache_file.write_text(json.dumps(cache), encoding="utf-8")
        except OSError:
            pass
    return runs, skipped


# ------------------------------------------------------------------ the tool

def build_tool(asan):
    out = REPO / "build" / ("host-js-wsl" if in_wsl() else "host-js")
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("t262tool-asan" if asan else "t262tool")
    srcs = sorted((JS / "src").glob("*.c")) + [REPO / "tests" / "host" / "t262tool.c"]
    deps = srcs + sorted((JS / "src").glob("*.h")) + sorted((JS / "include").glob("*.h")) + [REPO / "tests" / "host" / "modload.h"]
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = shutil.which(os.environ.get("CC", "gcc")) or shutil.which("cc") or shutil.which("clang")
    if not cc:
        sys.exit("no C compiler")
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if asan else ["-O2", "-g"]
    cmd = [cc, *flags, "-std=gnu17", "-w", "-I", str(JS / "include"), *map(str, srcs), "-lm", "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build failed:\n" + r.stdout + r.stderr[-4000:])
    return exe


def run_shard(exe, harness, runs, workdir, idx, limit_ms, asan):
    """Runs a list of runs through the tool; returns {id: (status, message)}.  Crashes and hangs are recorded."""
    results = {}
    pending = list(runs)
    attempt = 0
    while pending:
        attempt += 1
        mf = workdir / ("shard%d_%d.tsv" % (idx, attempt))
        mf.write_text("".join("\t".join(r) + "\n" for r in pending), encoding="utf-8")
        cmd = [str(exe), "--time-limit", str(limit_ms), str(harness), str(mf)]
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        errfile = workdir / ("shard%d_%d.err" % (idx, attempt))
        errf = open(errfile, "w")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=errf, text=True, errors="replace", env=env)
        state = {"last": time.time(), "killed": False}

        def watchdog():
            while proc.poll() is None:
                time.sleep(1)
                if time.time() - state["last"] > limit_ms / 1000 + 30:
                    state["killed"] = True
                    proc.kill()
                    return
        threading.Thread(target=watchdog, daemon=True).start()
        started = None
        for line in proc.stdout:
            state["last"] = time.time()
            parts = line.rstrip("\n").split("\t")
            if parts[0] == "B" and len(parts) >= 2:
                started = parts[1]
            elif parts[0] == "R" and len(parts) >= 3:
                results[parts[1]] = (parts[2], parts[3] if len(parts) > 3 else "")
                started = None
        rc = proc.wait()
        errf.close()
        err = errfile.read_text(errors="replace")
        errfile.unlink()
        mf.unlink()
        done = {r[0] for r in pending if r[0] in results}
        pending = [r for r in pending if r[0] not in results]
        if started and started not in results:
            why = "hang (killed)" if state["killed"] else "crash (exit %d) %s" % (rc, err.strip().splitlines()[0][:160] if err.strip() else "")
            if asan and "ERROR: AddressSanitizer" in err:
                m = re.search(r"ERROR: AddressSanitizer: (.*)", err)
                why = "asan: " + (m.group(1)[:120] if m else "")
            if "runtime error" in err:
                m = re.search(r"runtime error: (.*)", err)
                why = "ubsan: " + (m.group(1)[:120] if m else "")
            results[started] = ("CRASH", why)
            pending = [r for r in pending if r[0] != started]
        elif pending and not done:
            for r in pending:                       # the tool died before starting anything
                results[r[0]] = ("CRASH", "tool failure (exit %d) %s" % (rc, err.strip()[:160]))
            pending = []
    return results


def run_all(exe, harness, runs, jobs, limit_ms, asan, progress):
    workdir = REPO / "build" / "t262-work"
    workdir.mkdir(parents=True, exist_ok=True)
    size = 150
    shards = [runs[i:i + size] for i in range(0, len(runs), size)]
    results = {}
    lock = threading.Lock()
    t0 = time.time()

    def work(k):
        res = run_shard(exe, harness, shards[k], workdir, k, limit_ms, asan)
        with lock:
            results.update(res)
            if progress and (len(results) // size) % 20 == 0:
                print("  ... %d of %d runs done, %.0fs" % (len(results), len(runs), time.time() - t0), file=sys.stderr, flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(work, range(len(shards))))
    return results


# ------------------------------------------------------------------ reporting

def group_of(test_id, depth):
    parts = test_id.split("#")[0].split("/")
    return "/".join(parts[:depth]) if len(parts) > depth else "/".join(parts[:-1]) or parts[0]


def summarize(results, skipped, depth):
    """per-test verdict: a test passes when every run of it passes"""
    per_test = collections.OrderedDict()
    for rid, (st, msg) in sorted(results.items()):
        path = rid.split("#")[0]
        cur = per_test.get(path)
        if cur is None or (cur[0] == "PASS" and st != "PASS"):
            per_test[path] = (st, msg, rid)
    table = collections.OrderedDict()
    for path, (st, msg, rid) in per_test.items():
        g = group_of(path, depth)
        row = table.setdefault(g, collections.Counter())
        row[st] += 1
    for path, why in skipped.items():
        row = table.setdefault(group_of(path, depth), collections.Counter())
        row["SKIP"] += 1
    return per_test, table


def print_table(table, title):
    print("\n%s" % title)
    print("%-52s %7s %7s %7s %7s %8s" % ("directory", "pass", "fail", "crash", "skipped", "rate"))
    tot = collections.Counter()
    for g, row in table.items():
        ran = row["PASS"] + row["FAIL"] + row["CRASH"] + row["TIMEOUT"]
        rate = "%5.1f%%" % (100.0 * row["PASS"] / ran) if ran else "     -"
        print("%-52s %7d %7d %7d %7d %8s" % (g, row["PASS"], row["FAIL"], row["CRASH"] + row["TIMEOUT"], row["SKIP"], rate))
        tot.update(row)
    ran = tot["PASS"] + tot["FAIL"] + tot["CRASH"] + tot["TIMEOUT"]
    print("%-52s %7d %7d %7d %7d %8s" % ("TOTAL", tot["PASS"], tot["FAIL"], tot["CRASH"] + tot["TIMEOUT"], tot["SKIP"],
                                       "%5.1f%%" % (100.0 * tot["PASS"] / ran) if ran else "-"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="directories or files below test/ (default: %s)" % " ".join(DEFAULT_ROOTS))
    ap.add_argument("--dir", help="the test262 checkout")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4) // 2))
    ap.add_argument("--time-limit", type=int, default=10000, help="milliseconds per run (default 10000)")
    ap.add_argument("--asan", action="store_true", help="build the tool with AddressSanitizer and UBSan")
    ap.add_argument("--depth", type=int, default=2, help="directory depth of the table (default 2: built-ins/Array)")
    ap.add_argument("--no-skip", action="store_true", help="run tests of unsupported features too")
    ap.add_argument("--list", choices=["fail", "all"], help="print the failing tests (or every verdict) with their messages")
    ap.add_argument("--json", help="write every verdict to this file")
    ap.add_argument("--compare", help="a --json file of an earlier run: print what changed")
    ap.add_argument("--filter", help="only tests whose path contains this text")
    args = ap.parse_args()
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if not wsl:
            sys.exit("test262.py needs WSL (or Linux) for the host C tools")
        drive = REPO.drive[:1].lower()
        root = "/mnt/%s/%s" % (drive, str(REPO)[3:].replace("\\", "/"))
        quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in sys.argv[1:])
        script = "cd '%s' && PYTHONUNBUFFERED=1 python3 tests/host/test262.py %s" % (root, quoted)
        sys.exit(subprocess.call([wsl, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "sh", "-c", script]))
    t262 = find_test262(args.dir)
    roots = args.paths or DEFAULT_ROOTS
    test_root = t262 / "test"
    runs, skipped = plan(test_root, roots, UNSUPPORTED_FEATURES, args.no_skip)
    if args.filter:
        runs = [r for r in runs if args.filter in r[0]]
        skipped = {k: v for k, v in skipped.items() if args.filter in k}
    print("test262 at %s: %d runs planned, %d tests skipped" % (t262, len(runs), len(skipped)), file=sys.stderr)
    exe = build_tool(args.asan)
    t0 = time.time()
    results = run_all(exe, t262 / "harness", runs, args.jobs, args.time_limit, args.asan, True)
    per_test, table = summarize(results, skipped, args.depth)
    print_table(table, "test262 results (%s, %.0fs, %d runs)" % ("AddressSanitizer" if args.asan else "optimized", time.time() - t0, len(runs)))
    top = collections.OrderedDict()
    _, top_table = summarize(results, skipped, 1)
    print_table(top_table, "by top-level directory")
    reasons = collections.Counter(v for v in skipped.values())
    print("\nskipped: %s" % ", ".join("%s %d" % (k, v) for k, v in reasons.most_common(12)))
    if args.list:
        for path, (st, msg, rid) in per_test.items():
            if args.list == "all" or st != "PASS":
                print("%s\t%s\t%s\t%s" % (st, rid, msg[:200], ""))
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({k: list(v) for k, v in results.items()}, indent=0), encoding="utf-8")
    if args.compare:
        old = json.loads(Path(args.compare).read_text(encoding="utf-8"))
        gained = sorted(k for k, v in results.items() if v[0] == "PASS" and old.get(k, ["?"])[0] != "PASS")
        lost = sorted(k for k, v in results.items() if v[0] != "PASS" and old.get(k, [""])[0] == "PASS")
        print("\nsince %s: %d runs newly pass, %d runs newly fail" % (args.compare, len(gained), len(lost)))
        for k in lost[:50]:
            print("  now failing: %s  %s" % (k, results[k][1][:140]))
    crashes = [k for k, v in results.items() if v[0] == "CRASH"]
    if crashes:
        print("\n%d runs crashed the tool (a bug in the engine, never acceptable):" % len(crashes))
        for k in sorted(crashes)[:30]:
            print("  %s: %s" % (k, results[k][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
