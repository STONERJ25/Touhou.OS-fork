#!/usr/bin/env python3
"""The oracle of the Linux personality: builds the test programs with gcc on a real Linux, runs them, and stores what they print.

    wsl.exe -e python3 tools/linux-oracle.py                write tests/linux/golden/NAME.txt for every program
    wsl.exe -e python3 tools/linux-oracle.py --check        compare what this Linux prints now with the stored files (exit status 1 on a difference)
    wsl.exe -e python3 tools/linux-oracle.py --inputs       only build the statically linked programs into build/linux-inputs (the guest tests fetch them from there)
    ORACLE_ONLY=glibc_rtsig wsl.exe -e python3 tools/linux-oracle.py     (any of the above) limited to the named programs

This script runs ON a Linux (WSL on the development PC). Two kinds of program:

  * user/linuxtest/*.c: freestanding, no C library, only system calls (user/linuxtest/lnx.h). These are built into the initrd for TouhouOS as well.
  * tests/linux/src/*.c: ordinary C programs, linked statically with the C library of the machine this runs on. The binaries are test inputs: they are built into
    build/linux-inputs (git-ignored), never committed and never put into an image, and the guest test downloads them from the host (docs/COMPAT.md, "Test inputs").
  * tests/linux/py/*.py (L3): Python scripts, run by the python3 of this machine with a fixed environment (the PY_PROGRAMS table below); the scripts are copied to
    build/linux-inputs/py for the guest tests to download (tests/case_linux_py.py). ORACLE_ONLY=py_arith limits a run to one script.

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
HOSTED_FLAGS = ["-O1", "-Wall", "-pthread"]
STATIC_FLAGS = ["-static"]                                      # the default for a hosted program: the L1 corpus is linked statically

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
    # L2 (dynamic linking, docs/COMPAT.md): the same source in the three linking styles
    "dyn_hello": (["alpha", "beta"], {"FOO": "bar"}),
    "dyn_hello_nopie": (["alpha", "beta"], {"FOO": "bar"}),
    "dyn_hello_spie": (["alpha", "beta"], {"FOO": "bar"}),
    # a library opened at run time (LD_LIBRARY_PATH names the directory the oracle builds libdynplug.so in), and programs that start programs
    "dyn_dlopen": ([], {"LD_LIBRARY_PATH": "@INPUTS@"}),
    "dyn_spawn": ([], {}),
    # the L1 corpus again, as dynamically linked position-independent programs: the output must be the same as when they are static
    "dyn_libc": (["alpha", "beta"], {"FOO": "bar"}),
    "dyn_dir": ([], {}),
    "dyn_sys": ([], {}),
    "dyn_signals": ([], {}),
    "dyn_threads": ([], {}),
    # the memory-mapping calls of a dynamic linker, one at a time
    "map_tour": ([], {}),
}
HOSTED_PROGRAMS.update({                            # sockets, poll, epoll, eventfd, timerfd (tests/case_linux_net.py); they talk over loopback only
    "net_tcp": ([], {}),
    "net_errors": ([], {}),
    "net_udp": ([], {}),
    "net_unix": ([], {}),
    "net_unix2": ([], {}),
    "net_resolve": ([], {}),
    "net_tcp6": ([], {}),                            # IPv6 over ::1 (tests/case_linux_net.py: linux_net6_matches_linux)
    "net_poll": ([], {}),
    "net_epoll": ([], {}),
    "net_events": ([], {}),
    "net_host": ([], {}),
})
NO_RUN = {"net_host"}                               # built for the guest but not run here: the program talks to servers that tests/case_linux_net.py starts on the host

HOSTED_PROGRAMS.update({                            # real-time signals, who sent a signal, timers, pthread_cancel (tests/case_linux_signals.py)
    "glibc_rtsig": ([], {}),
    "glibc_siginfo": ([], {}),
    "glibc_timers": ([], {}),
    "glibc_cancel": ([], {}),
    "glibc_setxid": ([], {}),
    "glibc_sigsys": ([], {}),                       # signal 31 as an ordinary signal for a Linux program (freed on 2026-11-09)
})
HOSTED_PROGRAMS.update({                            # the Linux view of /proc as facts about its formats, static and dynamic (tests/case_linux_proc.py)
    "proc_probe": (["alpha", "beta"], {"FOO": "bar"}),
    "dyn_proc_probe": (["alpha", "beta"], {"FOO": "bar"}),
})

# How a hosted program is built when it is not simply `gcc -static NAME.c`: name -> (source file, flags). gcc's default is a dynamic position-independent executable.
RECIPES = {
    "dyn_hello": ("dyn_hello.c", ['-DKIND="dynamic PIE"']),
    "dyn_hello_nopie": ("dyn_hello.c", ["-no-pie", '-DKIND="dynamic non-PIE"']),
    "dyn_hello_spie": ("dyn_hello.c", ["-static-pie", '-DKIND="static-PIE"']),
    "dyn_dlopen": ("dyn_dlopen.c", []),
    "dyn_spawn": ("dyn_spawn.c", []),
    "dyn_libc": ("glibc_libc.c", []),
    "dyn_dir": ("glibc_dir.c", []),
    "dyn_sys": ("glibc_sys.c", []),
    "dyn_signals": ("glibc_signals.c", []),
    "dyn_threads": ("glibc_threads.c", []),
    "dyn_proc_probe": ("proc_probe.c", []),
}
# shared libraries: name -> (source file, flags); built beside the programs
LIBRARIES = {
    "libdynplug.so": ("dyn_plugin.c", ["-shared", "-fPIC"]),
}
# shell scripts: name -> file in tests/linux/scripts. Each is run by /bin/sh in an empty directory with PATH as the only variable, standard error mixed into standard output; the
# guest runs the same file with the shells of the Linux root (tests/case_linux_dyn.py), so these are comparisons of the root's programs with the real ones.
SCRIPTS = {
    "sh_files": "sh_files.sh",
    "sh_pipes": "sh_pipes.sh",
    "sh_env": "sh_env.sh",
}
SCRIPT_ENV = {"PATH": "/usr/bin:/bin"}
# programs that are only built for the guest tests: what they print is about the Linux root of TouhouOS, which a real Linux does not have, so nothing is stored for them
INPUT_ONLY = ["root_paths"]
SCRIPTS_DIR = ROOT / "tests" / "linux" / "scripts"

# L3 (docs/COMPAT.md, "State: L3"): Python scripts in tests/linux/py, one per area of what a real program uses. name -> (arguments, extra environment). Each is run by the python3 of
# this machine (3.12.3 on the development PC) in an empty working directory with exactly this environment: the hash seed, the locale, the zone and the buffering are fixed so that the
# same script prints the same text on any Linux with the same Python; standard error is mixed into standard output. The guest runs the same file with the python3 of the Linux root
# (tests/case_linux_py.py) and compares. The stored output is tests/linux/golden/py_NAME.txt. The scripts print only what the Python documentation promises (no addresses, process ids,
# times, paths of the machine or numbers that depend on the kernel's version, the number of processors or the user running them).
PY_DIR = ROOT / "tests" / "linux" / "py"
PYTHON = "/usr/bin/python3"
PY_VERSION = "Python 3.12.3"                       # the stored outputs are this Python's; another one makes the oracle skip the scripts (a check on a machine with a different Python is no failure)
PY_ENV = {"PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8", "PYTHONHASHSEED": "0", "PYTHONUNBUFFERED": "1", "HOME": "/tmp", "TZ": "UTC"}
PY_PROGRAMS = {name: ([], {}) for name in (
    "hello",                                       # the interpreter starts, finds its library, imports what a program imports
    "arith", "strfmt", "jsonre", "dates", "rng",   # the language and the pure-Python library: numbers, text, structured data, time, random numbers
    "hashing", "compress", "unittests",            # hashlib and friends, zlib/gzip/bz2/lzma/zipfile/tarfile, unittest
    "ffi", "sqldb",                                # ctypes (libffi) and sqlite3: shared libraries and database files
    "osops", "oscalls", "subproc",                 # the os module (files, descriptors, fork, exec), the less common calls, subprocess
    "signals", "clock", "mmaps",                   # signals and timers, time passing, mmap
    "threads", "procs",                            # threading and concurrent.futures, multiprocessing
    "sockets", "polling", "aio",                   # sockets over loopback, select/selectors/epoll, asyncio
)}


# L3, the C compiler (docs/COMPAT.md, "State: L3"; tests/case_linux_py.py, linux_py_gcc_*): the sources in tests/linux/src are compiled by the gcc of this machine in an empty directory and
# the stored text says what came out: the status, the size and the sha256 of the file the command makes (gcc is deterministic: the object file, the assembly and the linked program
# are the same bytes whenever the same gcc, binutils and C library make them), and the output of the program when it is run. name -> (arguments of gcc, the file it makes, the
# arguments to run that file with or None). The guest runs the same commands with the gcc of the Linux root and must get the same text.
GCC_VERSION = "gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"       # the first line of `gcc --version`: the stored text is this compiler's
GCC_SOURCES = ["cc_hello.c", "cc_calc.c"]
GCC_CASES = {
    "gcc_hello_object": (["-c", "cc_hello.c", "-o", "cc_hello.o"], "cc_hello.o", None),
    "gcc_calc_object": (["-O2", "-c", "cc_calc.c", "-o", "cc_calc.o"], "cc_calc.o", None),
    "gcc_calc_assembly": (["-O1", "-S", "cc_calc.c", "-o", "cc_calc.s"], "cc_calc.s", None),
    "gcc_hello_program": (["cc_hello.c", "-o", "cc_hello"], "cc_hello", ["alpha", "beta gamma"]),
    "gcc_calc_program": (["-O2", "cc_calc.c", "-o", "cc_calc", "-lm"], "cc_calc", []),
}


def source_of(name, hosted):
    if hosted:
        return HOSTED / RECIPES.get(name, (f"{name}.c",))[0]
    return FREESTANDING / f"{name}.c"


def build_library(name, outdir):
    src, extra = LIBRARIES[name]
    out = Path(outdir) / name
    subprocess.run(["gcc", *HOSTED_FLAGS, *extra, "-o", str(out), str(HOSTED / src)], check=True)
    return out


def build(name, outdir, hosted):
    out = Path(outdir) / name
    if hosted:
        _, extra = RECIPES.get(name, (None, STATIC_FLAGS))
        cmd = ["gcc", *HOSTED_FLAGS, *extra, "-o", str(out), str(source_of(name, True)), "-lm"]
    else:
        cmd = ["gcc", *FREE_FLAGS, "-o", str(out), str(FREESTANDING / f"{name}.c")]
    subprocess.run(cmd, check=True)
    return out


def run(binary, args, env):
    env = {k: v.replace("@INPUTS@", str(INPUTS)) for k, v in env.items()}       # a program that opens a library next to the inputs is told where they are
    r = subprocess.run([str(binary), *args], env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    return r.stdout.decode("utf-8", "replace").replace("\r", ""), r.returncode


def main():
    check = "--check" in sys.argv
    inputs_only = "--inputs" in sys.argv
    only = [n for n in os.environ.get("ORACLE_ONLY", "").split(",") if n]       # ORACLE_ONLY=a,b: just these programs (their stored output is rewritten, the others are left alone)
    GOLDEN.mkdir(parents=True, exist_ok=True)
    bad = 0
    groups = [(HOSTED_PROGRAMS, True)] if inputs_only else [(FREE_PROGRAMS, False), (HOSTED_PROGRAMS, True)]
    INPUTS.mkdir(parents=True, exist_ok=True)
    for lib in LIBRARIES:                                           # the libraries sit beside the programs in the inputs, for the guest to download as well
        out = build_library(lib, INPUTS)
        print(f"linux-oracle: {lib}: built {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB)")
    for name in INPUT_ONLY:
        out = build(name, INPUTS, True)
        print(f"linux-oracle: {name}: built {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB), no stored output")
    with tempfile.TemporaryDirectory(prefix="lnx-oracle-") as tmp:
        for programs, hosted in groups:
            for name, (args, env) in programs.items():
                if only and name not in only:
                    continue
                src = source_of(name, hosted)
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
                if name in NO_RUN:
                    print(f"linux-oracle: {name}: built, not run")
                    continue
                out, rc = run(binary, args, dict(env))
                bad += record(name, out + f"exit status: {rc}\n", check, rc)
        for name, script in SCRIPTS.items():
            INPUTS.mkdir(parents=True, exist_ok=True)
            (INPUTS / script).write_bytes((SCRIPTS_DIR / script).read_bytes())        # the guest downloads the script it runs
            if inputs_only:
                continue
            work = Path(tmp) / f"work-{name}"
            work.mkdir()
            r = subprocess.run(["/bin/sh", str(SCRIPTS_DIR / script)], cwd=work, env=dict(SCRIPT_ENV), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=120)
            bad += record(name, r.stdout.decode("utf-8", "replace").replace("\r", "") + f"exit status: {r.returncode}\n", check, r.returncode)
        (INPUTS / "py").mkdir(parents=True, exist_ok=True)
        for name in PY_PROGRAMS:                                    # the guest downloads the scripts it runs (build/linux-inputs/py/NAME.py)
            (INPUTS / "py" / f"{name}.py").write_bytes((PY_DIR / f"{name}.py").read_bytes())
        if not inputs_only:
            version = python_version()
            if version != PY_VERSION:
                print(f"linux-oracle: the python3 of this machine is {version!r}, the stored outputs of the scripts are those of {PY_VERSION!r}: the Python scripts are not run")
            else:
                for name, (args, extra) in PY_PROGRAMS.items():
                    if only and f"py_{name}" not in only:
                        continue
                    work = Path(tmp) / f"work-py-{name}"
                    work.mkdir()
                    out, rc = run_python(PY_DIR / f"{name}.py", args, extra, work)
                    bad += record(f"py_{name}", out + f"exit status: {rc}\n", check, rc)
        (INPUTS / "cc").mkdir(parents=True, exist_ok=True)
        for src in GCC_SOURCES:                                     # the guest compiles these
            (INPUTS / "cc" / src).write_bytes((HOSTED / src).read_bytes())
        if not inputs_only:
            version = gcc_version()
            if version != GCC_VERSION:
                print(f"linux-oracle: the gcc of this machine is {version!r}, the stored outputs of the compiler tests are those of {GCC_VERSION!r}: they are not run")
            else:
                for name, (args, produces, run_args) in GCC_CASES.items():
                    if only and name not in only:
                        continue
                    work = Path(tmp) / f"work-{name}"
                    work.mkdir()
                    bad += record(name, run_gcc_case(args, produces, run_args, work), check, 0)
    return 1 if bad else 0


def gcc_version():
    """The first line of `gcc --version`, or "" when there is no gcc."""
    try:
        r = subprocess.run(["gcc", "--version"], capture_output=True, text=True, timeout=60)
        return r.stdout.splitlines()[0].strip() if r.returncode == 0 and r.stdout else ""
    except OSError:
        return ""


def run_gcc_case(args, produces, run_args, work):
    """Compiles the sources of GCC_SOURCES in `work` as `gcc ARGS` and describes what came out, as the guest describes what it got: the command, its status and messages, the size and
    sha256 of the file it made, and for a program the command that ran it, its output and its status."""
    import hashlib
    for src in GCC_SOURCES:
        (work / src).write_bytes((HOSTED / src).read_bytes())
    env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
    r = subprocess.run(["gcc", *args], cwd=work, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=600)
    lines = [f"$ gcc {' '.join(args)}", f"status: {r.returncode}"] + (r.stdout + r.stderr).splitlines()
    made = work / produces
    if made.is_file():
        data = made.read_bytes()
        lines.append(f"{produces}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
        if run_args is not None:
            made.chmod(0o755)
            p = subprocess.run([f"./{produces}", *run_args], cwd=work, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
            shown = " ".join("'" + a + "'" if " " in a else a for a in run_args)
            lines.append(f"$ ./{produces} {shown}".rstrip())
            lines += p.stdout.decode("utf-8", "replace").replace("\r", "").splitlines()
            lines.append(f"exit status: {p.returncode}")
    return "\n".join(lines) + "\n"


def python_version():
    """What the python3 of this machine calls itself ("Python 3.12.3"), or "" when there is none."""
    try:
        r = subprocess.run([PYTHON, "--version"], capture_output=True, text=True, timeout=60)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


def run_python(script, args, extra, work):
    """Runs a script of tests/linux/py with the fixed environment of PY_ENV in the directory `work`: (standard output and standard error mixed, exit status)."""
    env = dict(PY_ENV)
    env.update(extra)
    r = subprocess.run([PYTHON, str(script), *args], cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    return r.stdout.decode("utf-8", "replace").replace("\r", ""), r.returncode


def record(name, text, check, rc):
    """Stores the output of a real Linux as tests/linux/golden/NAME.txt, or (--check) compares it with the stored file. Returns 1 for a difference."""
    path = GOLDEN / f"{name}.txt"
    if check:
        want = path.read_text(encoding="utf-8") if path.is_file() else ""
        if want != text:
            print(f"linux-oracle: {name}: the stored output differs from this Linux:")
            sys.stdout.writelines(difflib.unified_diff(want.splitlines(True), text.splitlines(True), "stored", "this Linux"))
            return 1
        print(f"linux-oracle: {name}: same ({len(text.splitlines())} lines)")
        return 0
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"linux-oracle: {name}: {len(text.splitlines())} lines written to {path.relative_to(ROOT)} (exit status {rc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
