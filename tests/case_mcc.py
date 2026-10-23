"""mcc, Marisa's compiler (with mld, her linker, and mar, her archiver).  See docs/MCC.md.

The host copy of mcc is built with the Linux gcc of WSL (tests/mcc_util.py); everything it produces is a TouhouOS ELF file
that only ever runs in QEMU.  The guest runs of this file:

  mcc_corpus           the programs of tests/mcc, compiled on the host, print the expected text in the guest
  mcc_on_guest         mcc running ON the guest compiles hello, stdio, math, pthread, socket and process programs
  mcc_selfhost         mcc compiles mcc: stage 2 (host) and the guest's stage 3 are the same file
  mcc_kappabox         all of kappabox, built with mcc, passes the shell test table
  mld_relinks_clang    objects made by clang are relinked by mld and the programs still run
  mcc_libgap           libgap itself (all but math and setjmp) compiled by mcc passes the OS's own utest suite
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import test, Qemu  # noqa: E402
import mcc_util as U  # noqa: E402

ROOT = U.ROOT
CORPUS_DIR = U.CORPUS
GUEST_DIR = CORPUS_DIR / "guest"
NOT_RUN = {"pp1", "allhdr"}      # compared with clang -E / only parsed on the host
LLVM = Path(r"C:\Program Files\LLVM\bin")


def corpus():
    return sorted(p.stem for p in CORPUS_DIR.glob("*.c") if p.stem not in NOT_RUN)


def guest_programs():
    return sorted(p.stem for p in GUEST_DIR.glob("*.c"))


def prepare():
    """Host tools + system root; returns nothing, raises with a useful message if the build breaks."""
    initrd = U.BUILD / "initrd.img"
    newest = max(p.stat().st_mtime for p in (ROOT / "user" / "bin" / "mcc").rglob("*") if p.is_file())
    if not initrd.exists() or initrd.stat().st_mtime < newest:
        raise RuntimeError("build/initrd.img is older than the sources of mcc: run python tools/build.py first")
    U.build_host()
    U.make_sysroot()


def host_compile(src, out, args=(), cwd=ROOT):
    """Compiles and links one file with the host mcc; `out` may be absolute (a Windows path) or relative to cwd."""
    outp = Path(out)
    wsl_out = U.wsl_path(outp) if outp.is_absolute() else out
    rc, so, se = U.host("mcc", [*args, src, "-o", wsl_out], cwd=cwd)
    assert rc == 0, f"mcc failed on {src} (exit {rc}):\n{so}{se}"
    return outp if outp.is_absolute() else Path(cwd) / out


def expect_output(name, got, want):
    got, want = got.replace("\r", "").strip(), want.replace("\r", "").strip()
    if got != want:
        import difflib
        diff = "\n".join(list(difflib.unified_diff(want.splitlines(), got.splitlines(), "expected", "guest", lineterm=""))[:40])
        raise AssertionError(f"{name}: output differs\n{diff}")


# ----------------------------------------------------------------------------------------------------------- host only

@test
def mcc_preprocessor():
    """mcc -E: macros, #if, #include of every libgap header: the same tokens as clang -E (host only)."""
    prepare()
    inc = ["-I", "user/libc/include", "-I", "shared", "-I", "user/bin/mcc/include"]
    for name in ("tests/mcc/pp1.c", "tests/mcc/allhdr.c", "user/bin/hello.c"):
        rc, so, se = U.host("mcc", ["-E", *inc, name])
        assert rc == 0, f"mcc -E {name}: {se}"
        cl = subprocess.run([str(LLVM / "clang.exe"), "-E", "-P", "-undef", "-nostdinc", "-D__STDC_VERSION__=201112L", "-D__STDC__=1",
                             "-D__x86_64__=1", "-D__LP64__=1", "-D__MCC__=1", *inc, name], capture_output=True, text=True, cwd=ROOT)
        assert cl.returncode == 0, cl.stderr
        tok = lambda s: re.findall(r"[A-Za-z_0-9]+|\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'|\.\.\.|<<=|>>=|[-+*/%&|^<>=!]=|->|\+\+|--|&&|\|\||<<|>>|##|\S", s)
        a = [t.replace("__asm__", "asm") for t in tok(so)]
        b = [t.replace("__asm__", "asm") for t in tok(cl.stdout)]      # mcc spells __asm__ as the canonical keyword
        if a != b:
            i = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            raise AssertionError(f"{name}: token {i} differs: mcc {' '.join(a[max(0, i - 6):i + 8])} / clang {' '.join(b[max(0, i - 6):i + 8])}")


ERRORS = [
    ("int main(void) { return y; }", r"error: 'y' undeclared", "borrowed"),
    ("int main(void) { int x = ; return 0; }", r"error: expected an expression", "missing"),
    ("int main(void) { return 0 }", r"error: expected ';'", "missing"),
    ("int f(int a) { return a; }\nint main(void) { return f(1, 2); }", r"error: too many arguments", None),
    ("int f(int a, int b) { return a; }\nint main(void) { return f(1); }", r"error: too few arguments", None),
    ("struct S { int a; };\nint main(void) { struct S s; return s.nope; }", r"error: no such member", None),
    ("int main(void) { const int c = 1; c = 2; return c; }", r"error: assignment of read-only", None),
    ("int main(void) { int a[3]; a = 0; return 0; }", r"error: not an lvalue", None),
    ("#include <nosuchheader.h>\nint main(void) { return 0; }", r"nosuchheader.h.*file not found", "shelves"),
    ("#error this is on purpose\nint main(void) { return 0; }", r"error: #error this is on purpose", None),
    ("int main(void) { char *s = \"unclosed; return 0; }", r"error: unclosed string literal", "open"),
    ("int main(void) { goto nowhere; return 0; }", r"error: use of undeclared label", None),
    ("void f(void) { break; }", r"error: stray break", None),
    ("int main(void) { int n = 3; int v[n]; return 0; }", r"variable length arrays are not supported", "not in my book"),
    ("_Static_assert(sizeof(int) == 5, \"int is four bytes\");", r"static assertion failed", None),
    ("int x = 1; int x = 2;", r"redefinition|redeclared|multiple", None),
]


@test
def mcc_diagnostics():
    """mcc: a mistake gets 'file:line: error: text', a caret line and one of Marisa's remarks; no object is written (host only)."""
    prepare()
    out = U.OUT
    out.mkdir(parents=True, exist_ok=True)
    problems = []
    for i, (code, pattern, remark) in enumerate(ERRORS):
        src = out / f"err{i}.c"
        src.write_text(code + "\n")
        obj = out / f"err{i}.o"
        if obj.exists():
            obj.unlink()
        rc, so, se = U.host("mcc", ["-c", f"build/mcc-out/err{i}.c", "-o", f"build/mcc-out/err{i}.o"])
        text = so + se
        if rc == 0 or not re.search(pattern, text, re.S):
            problems.append(f"#{i} {code!r}: rc={rc}\n{text}")
            continue
        if not re.search(rf"err{i}\.c:\d+: error: ", text):
            problems.append(f"#{i}: no 'file:line: error:' prefix in\n{text}")
        if "^" not in text or "Marisa" not in text:
            problems.append(f"#{i}: no caret line or remark in\n{text}")
        if remark and remark not in text:
            problems.append(f"#{i}: expected the remark '{remark}' in\n{text}")
        if obj.exists():
            problems.append(f"#{i}: an object file was written although the compile failed")
    assert not problems, "\n".join(problems)


@test
def mcc_object_files():
    """mcc -c writes valid ELF64 objects (llvm tools read them), mar writes archives llvm-ar and mld can use (host only)."""
    prepare()
    rc, so, se = U.host("mcc", ["-c", "tests/mcc/statics.c", "-o", "build/mcc-out/statics.o"])
    assert rc == 0, se
    obj = U.OUT / "statics.o"
    r = subprocess.run([str(LLVM / "llvm-readobj.exe"), "--file-headers", "--symbols", "--relocations", "--sections", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0 and "Machine: EM_X86_64" in r.stdout and "Type: Relocatable" in r.stdout, r.stdout + r.stderr
    for sym in ("main", "counter_user", "entries", "tl_counter"):
        assert re.search(rf"Name: {sym} ", r.stdout), f"symbol {sym} missing from the object"
    assert "R_X86_64_TPOFF32" in r.stdout and "R_X86_64_PLT32" in r.stdout and "R_X86_64_PC32" in r.stdout and "R_X86_64_64" in r.stdout
    d = subprocess.run([str(LLVM / "llvm-objdump.exe"), "-d", "--no-show-raw-insn", str(obj)], capture_output=True, text=True)
    assert d.returncode == 0 and "<main>:" in d.stdout and "ret" in d.stdout, d.stderr
    # mar: a two-member archive that llvm-ar lists and reads back
    for n in ("algo", "crypt"):
        rc, so, se = U.host("mcc", ["-c", f"tests/mcc/{n}.c", "-o", f"build/mcc-out/{n}.o"])
        assert rc == 0, se
    lib = U.OUT / "libt.a"
    if lib.exists():
        lib.unlink()
    rc, so, se = U.host("mar", ["rcs", "build/mcc-out/libt.a", "build/mcc-out/algo.o", "build/mcc-out/crypt.o"])
    assert rc == 0, se
    t = subprocess.run([str(LLVM / "llvm-ar.exe"), "t", str(lib)], capture_output=True, text=True)
    assert t.stdout.split() == ["algo.o", "crypt.o"], t.stdout + t.stderr
    s = subprocess.run([str(LLVM / "llvm-nm.exe"), "--print-armap", str(lib)], capture_output=True, text=True)
    armap = s.stdout.split("algo.o:")[0]       # only the globals of the members are indexed, statics are not
    assert "main in algo.o" in armap and "main in crypt.o" in armap and "crc32" not in armap, s.stdout
    rc, so, se = U.host("mar", ["t", "build/mcc-out/libt.a"])
    assert so.split() == ["algo.o", "crypt.o"], so + se


@test
def mcc_driver_options():
    """mcc: -D -U -I -include -E -o -c -s -M --version behave as documented (host only)."""
    prepare()
    out = U.OUT
    (out / "inc").mkdir(parents=True, exist_ok=True)
    (out / "inc" / "extra.h").write_text("#define FROM_EXTRA 7
int extra_value(void);
")
    (out / "pre.h").write_text("#define FROM_PRELUDE 11
")
    (out / "opts.c").write_text(
        "#include <extra.h>
#ifdef GONE
int gone = GONE;
#endif
"
        "int answer = ANSWER + FROM_EXTRA + FROM_PRELUDE;
#ifdef NAMED
const char *name = NAME;
#endif
")
    rc, so, se = U.host("mcc", ["-E", "-DANSWER=30", "-DGONE=1", "-UGONE", "-DNAMED", '-DNAME="marisa"', "-I", "build/mcc-out/inc",
                                "-include", "build/mcc-out/pre.h", "build/mcc-out/opts.c"])
    assert rc == 0, se
    flat = " ".join(so.split())
    assert "int answer = 30 + 7 + 11 ;" in flat and "gone" not in flat and 'const char *name = "marisa" ;' in flat, so
    # -E -o writes the file instead of stdout
    rc, so, se = U.host("mcc", ["-E", "-I", "build/mcc-out/inc", "-DANSWER=1", "build/mcc-out/opts.c", "-o", "build/mcc-out/opts.i"])
    assert rc == 0 and so == "" and "int answer" in (out / "opts.i").read_text(), (rc, so, se)
    # -c names the object after the source, -o overrides; the output is an ELF relocatable
    src = "tests/mcc/guest/hello.c"
    rc, so, se = U.host("mcc", ["-c", "-O2", "-Wall", "-g", "-std=c11", "-fPIC", src, "-o", "build/mcc-out/h.o"])
    assert rc == 0, se
    assert (out / "h.o").read_bytes()[:4] == b"ELF"
    # linking: -s strips, -M prints a map with main in it
    rc, so, se = U.host("mcc", [src, "-o", "build/mcc-out/h-sym"])
    assert rc == 0, se
    rc, so, se = U.host("mcc", [src, "-s", "-o", "build/mcc-out/h-strip"])
    assert rc == 0, se
    assert (out / "h-strip").stat().st_size < (out / "h-sym").stat().st_size
    rc, so, se = U.host("mcc", [src, "-M", "-o", "build/mcc-out/h-map"])
    assert rc == 0 and re.search(r"^[0-9a-f]{16} main$", so, re.M) and re.search(r"^[0-9a-f]{16} _start$", so, re.M), so
    elf = (out / "h-sym").read_bytes()
    assert elf[:4] == b"ELF" and elf[16:18] == b" " and elf[18:20] == b"> ", "not an x86-64 ET_EXEC"
    rc, so, se = U.host("mcc", ["--version"])
    assert rc == 0 and so.startswith("mcc ") and "Marisa" in so, so
    rc, so, se = U.host("mcc", ["-S", src])
    assert rc == 1 and "-S is not supported" in se, se
    rc, so, se = U.host("mcc", ["nonexistent-file.c"])
    assert rc == 1 and "cannot open" in se, se
    rc, so, se = U.host("mcc", ["-c", "build/mcc-out/h.o", "-o", "build/mcc-out/x.o"])
    assert rc == 1 and "not a C source file" in se, se
    # mld alone: the undefined reference is reported with the name and Marisa's remark
    (out / "undef.c").write_text("int missing(void);
int main(void) { return missing(); }
")
    rc, so, se = U.host("mcc", ["-c", "build/mcc-out/undef.c", "-o", "build/mcc-out/undef.o"])
    assert rc == 0, se
    rc, so, se = U.host("mcc", ["build/mcc-out/undef.o", "-o", "build/mcc-out/undef"])
    assert rc == 1 and "undefined reference to 'missing'" in se and "Marisa" in se, se
    # two definitions of a function
    (out / "dup1.c").write_text("int twice(void) { return 1; }
")
    (out / "dup2.c").write_text("int twice(void) { return 2; }
int main(void) { return twice(); }
")
    rc, so, se = U.host("mcc", ["build/mcc-out/dup1.c", "build/mcc-out/dup2.c", "-o", "build/mcc-out/dup"])
    assert rc == 1 and "multiple definition of 'twice'" in se, se


# ------------------------------------------------------------------------------------------------------------ the guest

def boot_with(extra, name, cmdline="text", **kw):
    esp = U.make_esp(name, extra)
    return Qemu(cmdline=cmdline, name=name, esp=esp, **kw)


@test
def mcc_corpus():
    """mcc: tests/mcc/*.c (arithmetic, pointers, structs, floats, varargs, switch, statics, calls, atomics...) built on the host print the expected text."""
    prepare()
    names = corpus()
    extra = {}
    for n in names:
        extra[f"mcc-tests/{n}"] = host_compile(f"tests/mcc/{n}.c", f"build/mcc-out/{n}")
    failures = []
    with boot_with(extra, "t-mcc-corpus") as vm:
        vm.expect(U.PROMPT, timeout=300)
        for n in names:
            try:
                out, rc = U.run(vm, f"/mcc-tests/{n}", timeout=180)
                assert rc == 0, f"exit status {rc}"
                expect_output(n, out, (CORPUS_DIR / f"{n}.out").read_text())
            except Exception as e:  # noqa: BLE001
                failures.append(str(e)[-1500:])
    assert not failures, "\n\n".join(failures)


def guest_run_all(vm, names, compiler="mcc", srcdir="/mcc-src"):
    """Compiles each program with the guest's compiler and runs it; returns the failures."""
    failures = []
    for n in names:
        try:
            out, rc = U.run(vm, f"{compiler} {srcdir}/{n}.c -o /tmp/{n} -lm -lpthread 2>&1; echo compile-rc=$?; /tmp/{n} alpha 'two words' 2>&1; echo run-rc=$?", timeout=600)
            assert "compile-rc=0" in out, f"compile failed:\n{out[-1500:]}"
            want = (GUEST_DIR / f"{n}.out").read_text()
            body = out.split("compile-rc=0\n", 1)[1]
            expect_output(n, body, want)
        except Exception as e:  # noqa: BLE001
            failures.append(f"{n}: {str(e)[-1800:]}")
    return failures


@test
def mcc_on_guest():
    """mcc ON the guest compiles and links hello, stdio, libm, pthread (TLS, atomics), socket and process programs, which then run."""
    prepare()
    extra = {f"mcc-src/{n}.c": GUEST_DIR / f"{n}.c" for n in guest_programs()}
    with boot_with(extra, "t-mcc-guest") as vm:
        vm.expect(U.PROMPT, timeout=300)
        out, rc = U.run(vm, "mcc --version; cc --version; mld --help 2>&1 | head -2; mar 2>&1 | head -1; ls -l /bin/cc /bin/mld /bin/mar", timeout=60)
        assert "Marisa" in out and out.count("mcc 0.1") >= 2 and "/bin/mld" in out, out
        failures = guest_run_all(vm, guest_programs())
        # errors on the guest: the exit status and the message
        out, rc = U.run(vm, "echo 'int main(void) { return nope; }' > /tmp/bad.c; mcc /tmp/bad.c -o /tmp/bad; echo rc=$?; ls /tmp/bad 2>&1", timeout=60)
        if not (re.search(r"bad\.c:1: error: 'nope' undeclared", out) and "rc=1" in out and "No such file" in out):
            failures.append("error handling on the guest:\n" + out)
        # -c and -o, then a separate link step with mld
        out, rc = U.run(vm, "cd /tmp && mcc -c /mcc-src/hello.c -o h.o && mld -o hello2 /ScarletDevilMansion/lib/crt0.o h.o /ScarletDevilMansion/lib/libgap.a && ./hello2 x; echo rc=$?", timeout=120)
        if "Hello from Gensokyo, compiled by Marisa!" not in out or "rc=3" not in out:
            failures.append("mcc -c followed by mld:\n" + out)
    assert not failures, "\n\n".join(failures)


SELFHOST_STEPS = (
    "cd /ScarletDevilMansion/src/mcc && "
    "/bin/mcc main.c -o /tmp/mcc2g && echo built-2g && "
    "/tmp/mcc2g main.c -o /tmp/mcc3 && echo built-3 && "
    "cmp /tmp/mcc2g /tmp/mcc3 && echo same-2g-3 && "
    "cmp /mcc-tests/mcc2 /tmp/mcc3 && echo same-host-2-and-3"
)


@test
def mcc_selfhost():
    """mcc compiles mcc: the host's stage 2, the guest's stage 2 and its stage 3 are the same file, and stage 3 compiles programs."""
    prepare()
    stage2 = host_compile("main.c", str(U.OUT / "mcc2"), cwd=ROOT / "user" / "bin" / "mcc")
    extra = {"mcc-tests/mcc2": stage2}
    extra.update({f"mcc-src/{n}.c": CORPUS_DIR / f"{n}.c" for n in ("algo", "crypt", "structs", "varargs")})
    failures = []
    with boot_with(extra, "t-mcc-selfhost") as vm:
        vm.expect(U.PROMPT, timeout=300)
        out, rc = U.run(vm, SELFHOST_STEPS, timeout=900)
        for step in ("built-2g", "built-3", "same-2g-3", "same-host-2-and-3"):
            if step not in out:
                failures.append(f"self-hosting stopped before '{step}':\n{out[-1500:]}")
                break
        else:
            for n in ("algo", "crypt", "structs", "varargs"):
                out, rc = U.run(vm, f"/tmp/mcc3 /mcc-src/{n}.c -o /tmp/{n}3 && /tmp/{n}3", timeout=300)
                try:
                    expect_output(n + " (compiled by stage 3)", out, (CORPUS_DIR / f"{n}.out").read_text())
                except AssertionError as e:
                    failures.append(str(e))
    assert not failures, "\n\n".join(failures)


@test
def mcc_kappabox():
    """The whole of kappabox (about 130 applets) compiled by mcc passes the shell test table."""
    prepare()
    import test_shell
    inc = ["-I", "user/bin/kappabox"]         # the headers of libgap and the user libraries come from the system root
    srcs = sorted(str(p.relative_to(ROOT)).replace("\\", "/") for p in (ROOT / "user" / "bin" / "kappabox").glob("*.c"))
    table = ROOT / "build" / "gen" / "kappabox_table.c"
    assert table.exists(), "build/gen/kappabox_table.c is missing: run tools/build.py first"
    rc, so, se = U.host("mcc", [*inc, *srcs, "build/gen/kappabox_table.c", "-lkappa", "-lfairy", "-laudio", "-limg", "-lz", "-o", "build/mcc-out/kappabox"], timeout=600)
    assert rc == 0, so + se
    failures = []
    with boot_with({"bin/kappabox": U.OUT / "kappabox"}, "t-mcc-kappabox") as vm:
        vm.expect(U.PROMPT, timeout=300)
        for cmd, expect, status in test_shell.CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                assert status is None or rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-500:]}")
    assert not failures, "\n".join(failures)


@test
def mld_relinks_clang():
    """mld relinks programs built by clang (crt0, objects, libraries, TLS, GOT-less static layout) and they run like the originals."""
    prepare()
    progs = {}
    for name in ("hello", "fstest"):
        cmdfile = ROOT / "build" / "user" / "bin" / f"{name}.cmd"
        assert cmdfile.exists(), f"{cmdfile} is missing: run tools/build.py first"
        words = cmdfile.read_text().split("\n")
        inputs = [w for w in words[words.index("-o") + 2:] if w]
        rel = []
        for w in inputs:
            p = Path(w)
            rel.append(str(p.relative_to(ROOT)).replace("\\", "/") if p.is_absolute() else w)
        rc, so, se = U.host("mld", ["-o", f"build/mcc-out/{name}-mld", *rel])
        assert rc == 0, f"mld failed for {name}: {so}{se}"
        progs[name] = U.OUT / f"{name}-mld"
    extra = {f"mcc-tests/{n}-mld": p for n, p in progs.items()}
    with boot_with(extra, "t-mld") as vm:
        vm.expect(U.PROMPT, timeout=300)
        out1, rc1 = U.run(vm, "/bin/hello one two", timeout=60)
        out2, rc2 = U.run(vm, "/mcc-tests/hello-mld one two", timeout=60)
        assert rc1 == rc2 == 0, (rc1, rc2)
        # the first line carries the pid and uid, argv[0] is the program's own name: compare the rest
        rest = lambda text: [ln for ln in text.splitlines()[1:] if "argv[0]" not in ln]
        assert rest(out1) == rest(out2) and len(rest(out2)) == 2 and "uid=1000" in out2.splitlines()[0], (out1, out2)
        out3, rc3 = U.run(vm, "/bin/fstest 2>&1 | tail -3", timeout=240)
        out4, rc4 = U.run(vm, "/mcc-tests/fstest-mld 2>&1 | tail -3", timeout=240)
        assert out3 == out4, (out3, out4)


@test
def mcc_libgap():
    """libgap compiled by mcc (every file but math.c and setjmp.asm) links with utest compiled by mcc and passes the OS's own utest suite."""
    prepare()
    import case_threads
    r = subprocess.run(["wsl.exe", "-e", "bash", U.wsl_path(ROOT / "tools" / "mcc_build_libgap.sh")], capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0 and "built build/mcc-libgap/libgap.a" in r.stdout, r.stdout + r.stderr
    env_root = U.wsl_path(ROOT / "build" / "mcc-root2")
    r = subprocess.run(["wsl.exe", "-e", "env", f"MCC_ROOT={env_root}", U.wsl_path(U.HOST / "mcc"), "user/bin/utest.c", "-lkappa", "-lfairy", "-laudio",
                        "-limg", "-lz", "-o", "build/mcc-out/utest"], capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    esp = U.make_esp("t-mcc-libgap", {"bin/utest": U.OUT / "utest"})
    with Qemu(cmdline="utest", name="t-mcc-libgap", esp=esp) as vm:
        vm.expect(r"UTEST (PASS|FAIL)", timeout=240)
        text = vm.text()
        assert "UTEST PASS" in text, text[-2500:]
        for section in case_threads.THREAD_SECTIONS:
            assert f"utest: threads: {section}" in text, f"thread section did not run: {section}"
        assert vm.wait_exit(10) == 0
