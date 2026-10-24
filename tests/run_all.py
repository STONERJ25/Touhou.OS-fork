#!/usr/bin/env python3
"""Scripted regression tests: boots the kernel in QEMU and checks serial output / guest behaviour.

  python tests/run_all.py                    run everything
  python tests/run_all.py usb                run the tests whose name starts with "usb"
  python tests/run_all.py usb,blk_,~clock    comma-separated filters; a leading ~ means "contains" instead of "starts with",
                                             a leading = means "exactly this name"
  python tests/run_all.py --retry-failed X   after the run, rerun each failed test alone once: one that passes then is
                                             reported as flaky and does not fail the run
  python tests/run_all.py --list X           print the names the filters select, run nothing
  python tests/run_all.py --json R.json X    also write the results (status and seconds per test) to R.json

Tests live in tests/case_*.py and register themselves with the @test decorator from harness.py; @xfail marks an
expected failure (reported as XFAIL, or XPASS when it passes; neither fails the run). Exit status 1 when a test failed
(after the retry, with --retry-failed) or a strict XPASS happened, else 0. docs/LINUX.md has the details.
"""
import argparse
import importlib
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import harness  # noqa: E402


def load_cases():
    """Imports every tests/case_*.py, which registers its tests in harness.TESTS (once)."""
    if not harness.TESTS:
        for module in sorted(HERE.glob("case_*.py")):
            importlib.import_module(module.stem)


def matches(name, wants):
    for w in wants:
        if w.startswith("~"):
            if w[1:] in name:
                return True
        elif w.startswith("="):
            if name == w[1:]:
                return True
        elif name.startswith(w):
            return True
    return False


def select(filters, tests=None):
    load_cases()
    wants = [w for w in (filters or "").split(",") if w] or [""]
    return [fn for fn in (harness.TESTS if tests is None else tests) if matches(fn.__name__, wants)]


def first_line(text):
    return (text.strip().splitlines() or [""])[0][:200]


def run_one(fn):
    """Runs one test and prints its report line. Returns a result dict: name, status (PASS FAIL XFAIL XPASS), seconds, error."""
    mark = getattr(fn, "xfail", None)
    expected = False
    if mark is not None:
        try:
            expected = mark.applies()
        except Exception as e:  # noqa: BLE001    a broken condition must not hide the test
            print(f"     (the @xfail condition of {fn.__name__} raised {e!r}: the test counts as an ordinary one)", flush=True)
    t0 = time.time()
    error, kind = "", ""
    try:
        fn()
        ok = True
    except Exception as e:  # noqa: BLE001
        ok, error, kind = False, str(e), type(e).__name__
    secs = time.time() - t0
    doc = fn.__doc__ or ""
    if expected:
        status = "XPASS" if ok else "XFAIL"
        print(f"{status} {fn.__name__:27} {secs:5.1f}s  {doc}\n     expected to fail: {mark.reason}", flush=True)
        if ok:
            print("     it passed: the @xfail condition is too wide for this environment" +
                  (" (strict: this fails the run)" if mark.strict else ""), flush=True)
        else:
            print(f"     {kind}: {first_line(error)}", flush=True)
    elif ok:
        status = "PASS"
        print(f"PASS {fn.__name__:28} {secs:5.1f}s  {doc}", flush=True)
    else:
        status = "FAIL"
        print(f"FAIL {fn.__name__:28} {secs:5.1f}s  {doc}\n     {error[:1500]}", flush=True)
    return {"name": fn.__name__, "status": status, "seconds": round(secs, 1), "error": error[:1500],
            "reason": mark.reason if expected else "", "strict": bool(expected and mark.strict)}


def retry_failed(results, by_name):
    """Reruns each FAIL alone, once, after the whole run. A pass turns the result into FLAKY (the first error is kept)."""
    failed = [r for r in results if r["status"] == "FAIL"]
    if not failed:
        return
    print(f"\nRetrying {len(failed)} failed test{'s' if len(failed) != 1 else ''} alone:", flush=True)
    for r in failed:
        again = run_one(by_name[r["name"]])
        r["retry_seconds"] = again["seconds"]
        if again["status"] == "PASS":
            r["status"] = "FLAKY"
            print(f"FLAKY {r['name']:27} failed in the run, passed alone", flush=True)
        else:
            r["retry_error"] = again["error"]
            print(f"FAIL {r['name']:28} failed again alone", flush=True)


def summarize(results, retried):
    count = {s: sum(1 for r in results if r["status"] == s) for s in ("FAIL", "FLAKY", "XFAIL", "XPASS")}
    head = f"{len(results)} tests, {count['FAIL']} failed"
    for key, word in (("FLAKY", "flaky"), ("XFAIL", "expected failures"), ("XPASS", "unexpected passes")):
        if count[key]:
            head += f", {count[key]} {word}"
    lines = [head]
    names = lambda s: ", ".join(r["name"] for r in results if r["status"] == s)  # noqa: E731
    if count["FAIL"]:
        lines.append(f"  failed{' (again when rerun alone)' if retried else ''}: {names('FAIL')}")
    if count["FLAKY"]:
        lines.append(f"  flaky (failed in the run, passed alone): {names('FLAKY')}")
    for r in results:
        if r["status"] in ("XFAIL", "XPASS"):
            lines.append(f"  {r['status'].lower()}: {r['name']} ({r['reason']})")
    return "\n".join(lines)


def exit_code(results):
    return 1 if any(r["status"] == "FAIL" or (r["status"] == "XPASS" and r["strict"]) for r in results) else 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("filters", nargs="?", default="", help="comma-separated name prefixes (~contains, =exact); empty runs everything")
    ap.add_argument("--retry-failed", action="store_true", help="rerun each failed test alone once; a pass then means flaky, not failed")
    ap.add_argument("--list", action="store_true", help="print the selected test names and exit")
    ap.add_argument("--json", metavar="FILE", help="write the results as JSON to FILE")
    args = ap.parse_args(argv)
    chosen = select(args.filters)
    if args.list:
        for fn in chosen:
            print(fn.__name__)
        return 0
    results = [run_one(fn) for fn in chosen]
    if args.retry_failed:
        retry_failed(results, {fn.__name__: fn for fn in chosen})
    print("\n" + summarize(results, args.retry_failed))
    code = exit_code(results)
    if args.json:
        Path(args.json).write_text(json.dumps({"filters": args.filters, "retry_failed": args.retry_failed, "exit": code,
                                               "results": results}, indent=1) + "\n", encoding="utf-8")
    return code


if __name__ == "__main__":
    sys.exit(main())
