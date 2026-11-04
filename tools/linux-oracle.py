#!/usr/bin/env python3
"""The oracle of the Linux personality: builds the test programs with gcc on a real Linux, runs them, and stores what they print.

    wsl.exe -e python3 tools/linux-oracle.py                write tests/linux/golden/NAME.txt for every program
    wsl.exe -e python3 tools/linux-oracle.py --check        compare what this Linux prints now with the stored files (exit status 1 on a difference)
    wsl.exe -e python3 tools/linux-oracle.py --inputs       only build the statically linked programs into build/linux-inputs (the guest tests fetch them from there)

This script runs ON a Linux (WSL on the development PC). Two kinds of program:

  * user/linuxtest/*.c: freestanding, no C library, only system calls (user/linuxtest/lnx.h). These are built into the initrd for TouhouOS as well.
  * tests/linux/src/*.c: ordinary C programs, linked statically with the C library of the machine this runs on. The binaries are test inputs: they are built into
    build/linux-inputs (git-ignored), never committed and never put into an image, and the guest test downloads them from the host (docs/COMPAT.md, "Test inputs").

The programs print only facts that the Linux manual pages promise, so the output of a real Linux is the specification of what the Linux personality of TouhouOS
must print for the same program: tests/case_linux.py compares them line by line, and tests/host/test_linux_abi.py re-runs --check so that a stored file cannot
drift away from the real thing. The sources are our own; gcc and the kernel under WSL are only used to learn the expected behaviour (observed behaviour is the
allowed source, docs/COMPAT.md).
"""
import difflib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FREESTANDING = ROOT / "user" / "linuxtest"
HOSTED = ROOT / "tests" / "linux" / "src"
GOLDEN = ROOT / "tests" / "linux" / "golden"
INPUTS = ROOT / "build" / "linux-inputs"
FREE_FLAGS = ["-O1", "-static", "-nostdlib", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-no-pie",
              "-fno-asynchronous-unwind-tables", "-Wall", "-Wno-unused-function", f"-I{FREESTANDING}"]
HOSTED_FLAGS = ["-O1", "-static", "-Wall", "-pthread"]

# name -> (arguments, environment); the environment is exactly this (nothing else is inherited)
FREE_PROGRAMS = {
    "lnx_hello": ([], {}),
    "lnx_probe": ([], {}),
    "lnx_args": (["alpha", "beta"], {"FOO": "bar"}),
}                                                   # lnx_enosys and lnx_exec are about this kernel's own gaps and paths: a real Linux has nothing to say about them
HOSTED_PROGRAMS = {
    "glibc_hello": ([], {}),
    "glibc_libc": (["alpha", "beta"], {"FOO": "bar"}),
    "glibc_dir": ([], {}),
    "glibc_sys": ([], {}),
    "glibc_signals": ([], {}),
    "glibc_threads": ([], {}),
}


def build(name, outdir, hosted):
    out = Path(outdir) / name
    if hosted:
        cmd = ["gcc", *HOSTED_FLAGS, "-o", str(out), str(HOSTED / f"{name}.c"), "-lm"]
    else:
        cmd = ["gcc", *FREE_FLAGS, "-o", str(out), str(FREESTANDING / f"{name}.c")]
    subprocess.run(cmd, check=True)
    return out


def run(binary, args, env):
    r = subprocess.run([str(binary), *args], env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    return r.stdout.decode("utf-8", "replace").replace("\r", ""), r.returncode


def main():
    check = "--check" in sys.argv
    inputs_only = "--inputs" in sys.argv
    GOLDEN.mkdir(parents=True, exist_ok=True)
    bad = 0
    groups = [(HOSTED_PROGRAMS, True)] if inputs_only else [(FREE_PROGRAMS, False), (HOSTED_PROGRAMS, True)]
    with tempfile.TemporaryDirectory(prefix="lnx-oracle-") as tmp:
        for programs, hosted in groups:
            for name, (args, env) in programs.items():
                src = (HOSTED if hosted else FREESTANDING) / f"{name}.c"
                if not src.is_file():
                    print(f"linux-oracle: {src.name} does not exist, skipped")
                    continue
                if inputs_only:
                    INPUTS.mkdir(parents=True, exist_ok=True)
                    out = build(name, INPUTS, True)
                    print(f"linux-oracle: {name}: built {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB)")
                    continue
                binary = build(name, tmp, hosted)
                if hosted:                                  # the guest downloads the very binary that produced the stored output
                    INPUTS.mkdir(parents=True, exist_ok=True)
                    (INPUTS / name).write_bytes(binary.read_bytes())
                out, rc = run(binary, args, dict(env))
                text = out + f"exit status: {rc}\n"
                path = GOLDEN / f"{name}.txt"
                if check:
                    want = path.read_text(encoding="utf-8") if path.is_file() else ""
                    if want != text:
                        bad += 1
                        print(f"linux-oracle: {name}: the stored output differs from this Linux:")
                        sys.stdout.writelines(difflib.unified_diff(want.splitlines(True), text.splitlines(True), "stored", "this Linux"))
                    else:
                        print(f"linux-oracle: {name}: same ({len(text.splitlines())} lines)")
                else:
                    path.write_text(text, encoding="utf-8", newline="\n")
                    print(f"linux-oracle: {name}: {len(text.splitlines())} lines written to {path.relative_to(ROOT)} (exit status {rc})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
