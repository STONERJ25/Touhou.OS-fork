#!/usr/bin/env python3
"""The whole userland, compiled by mcc on the host (a cross build: the programs are TouhouOS ELF files that only run in QEMU).

  python tools/mcc_userland.py                 libgap, the libraries and every program of user/bin and user/sbin -> build/mcc-user/
  python tools/mcc_userland.py --only bin/notebook,sbin/init     rebuild just those programs (libraries and objects of an earlier run are reused)
  python tools/mcc_userland.py --strict        fail when a unit had to fall back to the clang build (the default only reports it)

The layout mirrors tools/build.py: libgap from user/libc/src, one library per directory of user/lib, every program of user/bin
and user/sbin (the multicall kappabox with its generated applet table), the same sources and include paths.  Results:

  build/mcc-user/bin/NAME, sbin/NAME     the programs         (tools/mcc_userland_suites.py and tests/case_mcc.py put them into an initrd)
  build/mcc-user/root/                   a compiler system root whose libraries are the mcc-built ones (the link step uses it)
  build/mcc-user/report.json             what was built by mcc, what fell back to the clang objects, how long it took

A unit mcc cannot compile (libm's x87 assembly, the assembler sources before mas exists, ...) falls back to the object or program of the
clang build under build/ (run tools/build.py first) and is listed in the report: "entirely mcc" is the report with an empty fallback list.

The compiles run in parallel through tools/mcc_jobs.sh (WSL's Ubuntu on Windows: the host mcc is a Linux program, see tools/mcc_host.sh).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import mcc_util as U  # noqa: E402

BUILD = ROOT / "build"
OUT = BUILD / "mcc-user"

LIB_ORDER = ["kappa", "fairy"]                             # as in tools/build.py: link order does not matter to mld, but keep the names in step
LIB_EXTRA_SOURCES = {"kappa": ["shared/font8x16.c", "shared/yinyang.c"]}
ASM_UNITS = {                                                # the two NASM sources of libgap, assembled by mas (user/bin/mas)
    "crt0": "user/libc/crt0.asm",
    "setjmp": "user/libc/src/setjmp.asm",
}


def rel(p):
    return str(Path(p).resolve().relative_to(ROOT)).replace("\\", "/")


def run_wsl(args, **kw):
    """Runs a command in the Linux that hosts the compiler: WSL's Ubuntu on Windows, the machine itself elsewhere."""
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        if not wsl:
            sys.exit("mcc_userland: no wsl.exe: the host mcc is a Linux program (tools/mcc_host.sh)")
        args = [wsl, "-e", *args]
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors="replace", **kw)


def run_jobs(name, lines, par):
    """Writes the command lines to build/mcc-user/NAME.jobs and runs them in parallel; returns the failure text ('' when all passed)."""
    jf = OUT / f"{name}.jobs"
    jf.write_text("\n".join(lines) + "\n", newline="\n") if sys.version_info >= (3, 10) else jf.write_bytes(("\n".join(lines) + "\n").encode())
    r = run_wsl(["bash", "tools/mcc_jobs.sh", rel(jf), str(par)])
    return (r.stdout + r.stderr) if r.returncode else ""


def sources(base, patterns):
    out = []
    for pat in patterns:
        out += sorted(Path(base).glob(pat))
    return out


def lib_names():
    base = ROOT / "user" / "lib"
    names = sorted(p.name for p in base.iterdir() if p.is_dir() and (p / "src").is_dir())
    names.sort(key=lambda n: (LIB_ORDER.index(n) if n in LIB_ORDER else len(LIB_ORDER), n))
    return names


def programs():
    """[(kind, name, [sources], include_dir)] in the order of tools/build.py."""
    progs = []
    for kind in ("bin", "sbin"):
        d = ROOT / "user" / kind
        if not d.exists():
            continue
        for e in sorted(p for p in d.iterdir() if (p.suffix == ".c" and p.is_file()) or p.is_dir()):
            if e.is_file():
                progs.append((kind, e.stem, [e], e.parent))
            else:
                srcs = sources(e, ["**/*.c"])
                if not srcs:
                    continue
                if e.name == "kappabox":
                    table = BUILD / "gen" / "kappabox_table.c"
                    if not table.exists():
                        sys.exit("mcc_userland: build/gen/kappabox_table.c is missing: run python tools/build.py first")
                    srcs.append(table)
                progs.append((kind, e.name, srcs, e))
    return progs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma separated programs to (re)build, such as bin/notebook,sbin/init; needs an earlier full run")
    ap.add_argument("--strict", action="store_true", help="exit with status 1 when something fell back to the clang build")
    ap.add_argument("-j", "--jobs", type=int, default=0, help="parallel compiles (default: the cores of the host minus two)")
    ap.add_argument("--out", default="build/mcc-user", help="output directory, relative to the checkout (default build/mcc-user)")
    args = ap.parse_args()
    global OUT
    OUT = ROOT / args.out

    t0 = time.time()
    U.build_host()
    sysroot = U.make_sysroot()                             # build/mcc-root: headers for the compile step
    OUT.mkdir(parents=True, exist_ok=True)
    par = args.jobs or max(1, (os.cpu_count() or 4) - 2)
    host_mcc, host_mar = "build/host/mcc", "build/host/mar"
    report = {"fallback": [], "failed": [], "seconds": {}}
    only = set(args.only.split(",")) if args.only else None
    root = OUT / "root"

    if only is None:
        shutil.rmtree(OUT / "obj", ignore_errors=True)
        shutil.rmtree(OUT / "lib", ignore_errors=True)
        shutil.rmtree(root, ignore_errors=True)
        # ---- 1. compile every library unit
        units = []                                         # (group, source, object)
        for src in sources(ROOT / "user" / "libc" / "src", ["*.c"]):
            units.append(("libgap", src, OUT / "obj" / "libgap" / (src.stem + ".o")))
        for n in lib_names():
            base = ROOT / "user" / "lib" / n
            for src in sources(base, ["src/**/*.c"]) + [ROOT / s for s in LIB_EXTRA_SOURCES.get(n, [])]:
                units.append((n, src, OUT / "obj" / n / (rel(src).replace("/", "_") + ".o")))
        for g in {u[0] for u in units}:
            (OUT / "obj" / g).mkdir(parents=True, exist_ok=True)
        t = time.time()
        lines = [f"{host_mcc} -c -I {rel(src.parent)} {rel(src)} -o {rel(obj)}" for _, src, obj in units]
        failed = run_jobs("libs", [f"MCC_ROOT={U.wsl_path(sysroot)} {ln}" for ln in lines], par)
        report["seconds"]["libs-compile"] = round(time.time() - t, 1)
        # ---- units that did not compile fall back to the clang objects
        for g, src, obj in units:
            if obj.exists():
                continue
            clang = BUILD / "obj" / ("libc" if g == "libgap" else f"userlib/{g}") / (rel(src) + ".o")
            if clang.exists():
                shutil.copyfile(clang, obj)
                report["fallback"].append(rel(src))
            else:
                report["failed"].append(rel(src))
        # the NASM units
        (OUT / "obj" / "libgap").mkdir(parents=True, exist_ok=True)
        for name, asm in ASM_UNITS.items():
            obj = OUT / "obj" / "libgap" / (name + ".o")
            r = run_wsl(["build/host/mas", "-f", "elf64", asm, "-o", rel(obj)])
            if r.returncode or not obj.exists():
                clang = BUILD / "obj" / "libc" / (asm + ".o")
                print(f"mcc_userland: mas could not assemble {asm}: {r.stderr.strip()}")
                if not clang.exists():
                    sys.exit(f"mcc_userland: ... and {rel(clang)} is missing: run python tools/build.py first")
                shutil.copyfile(clang, obj)
                report["fallback"].append(asm)
        if failed:
            print(failed)
        # ---- 2. archives and the system root the programs link against
        shutil.copytree(sysroot, root)
        (root / "lib").mkdir(exist_ok=True)
        shutil.copyfile(OUT / "obj" / "libgap" / "crt0.o", root / "lib" / "crt0.o")
        (OUT / "lib").mkdir(exist_ok=True)
        for g in sorted({u[0] for u in units}):
            objs = sorted((OUT / "obj" / g).glob("*.o"))
            if g == "libgap":
                objs = [o for o in objs if o.name != "crt0.o"]
            lib = root / "lib" / ("libgap.a" if g == "libgap" else f"lib{g}.a")
            if lib.exists():
                lib.unlink()
            r = run_wsl([host_mar, "rcs", rel(lib), *[rel(o) for o in objs]])
            if r.returncode:
                sys.exit(f"mcc_userland: mar failed for {lib.name}: {r.stdout}{r.stderr}")
        report["libs"] = sorted(p.name for p in (root / "lib").glob("*.a"))
    elif not (root / "lib" / "libgap.a").exists():
        sys.exit("mcc_userland: --only needs the libraries of an earlier full run in build/mcc-user")

    # ---- 3. the programs: compile every source, then link
    progs = [p for p in programs() if only is None or f"{p[0]}/{p[1]}" in only]
    if only is None:
        shutil.rmtree(OUT / "bin", ignore_errors=True)
        shutil.rmtree(OUT / "sbin", ignore_errors=True)
    for kind in ("bin", "sbin"):
        (OUT / kind).mkdir(parents=True, exist_ok=True)
    t = time.time()
    lines, objs_of = [], {}
    for kind, name, srcs, inc in progs:
        objs_of[(kind, name)] = []
        odir = OUT / "obj" / f"{kind}-{name}"
        shutil.rmtree(odir, ignore_errors=True)
        odir.mkdir(parents=True)
        for i, src in enumerate(srcs):
            obj = odir / f"{i:03d}-{src.stem}.o"
            objs_of[(kind, name)].append(obj)
            lines.append(f"MCC_ROOT={U.wsl_path(root)} {host_mcc} -c -I {rel(inc)} {rel(src)} -o {rel(obj)}")
    failed = run_jobs("progs-compile", lines, par)
    report["seconds"]["programs-compile"] = round(time.time() - t, 1)
    t = time.time()
    link_lines = []
    for kind, name, srcs, inc in progs:
        objs = objs_of[(kind, name)]
        if not all(o.exists() for o in objs):
            continue
        libs = " ".join(f"-l{n}" for n in lib_names())
        link_lines.append(f"MCC_ROOT={U.wsl_path(root)} {host_mcc} {' '.join(rel(o) for o in objs)} {libs} -o {rel(OUT / kind / name)}")
    flink = run_jobs("progs-link", link_lines, par)
    report["seconds"]["programs-link"] = round(time.time() - t, 1)
    for kind, name, srcs, inc in progs:
        out = OUT / kind / name
        if out.exists():
            continue
        clang = BUILD / "user" / kind / name
        if clang.exists():
            shutil.copyfile(clang, out)
            report["fallback"].append(f"{kind}/{name}")
        else:
            report["failed"].append(f"{kind}/{name}")
    for text in (failed, flink):
        if text:
            print(text)
    report["programs"] = sorted(f"{k}/{p.name}" for k in ("bin", "sbin") for p in (OUT / k).glob("*") if p.is_file())
    report["seconds"]["total"] = round(time.time() - t0, 1)
    (OUT / "report.json").write_text(json.dumps(report, indent=1))
    print(f"mcc_userland: {len(report['programs'])} programs, {len(report.get('libs', []))} libraries in {report['seconds']['total']} s;"
          f" {len(report['fallback'])} fell back to the clang build, {len(report['failed'])} missing")
    for f in report["fallback"]:
        print("   clang:", f)
    for f in report["failed"]:
        print("   MISSING:", f)
    return 1 if report["failed"] or (args.strict and report["fallback"]) else 0


if __name__ == "__main__":
    sys.exit(main())
