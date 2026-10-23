"""diff and patch: every output format with exact expected text (checked against GNU diff), edge cases, exit statuses,
directories, patch offsets / rejects / reverse / dry run / -p, round trips on random files, and a size check.

Each check types one command into the guest shell over the serial console (~0.15 s) and compares the output exactly.
"""
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu

F1 = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n"
F2 = "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n"
LAB = "--label A --label B "

# files created in /tmp/d before the checks run
FILES = {
    "f1": F1, "f2": F2, "e": "", "x1": "a\nb\nc\n", "x2": "a\nc\n", "y1": "a\nb\n", "y2": "c\nd\n",
    "n1": "a\nb", "n2": "a\nb\n", "w1": "Hello  World \n", "w2": "hello world\n", "p": "a\n\nb\n", "q": "a\nb\n",
    "crlf": "a\r\nb\r\n", "lf": "a\nb\n", "s1": " a\n", "s2": "a\n",
    "g1": "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n",
    "g2": "1\n2\n3\nX\n5\n6\n7\n8\n9\n10\n11\nY\n13\n14\n15\n16\n17\n18\n19\n20\n",
    "g3": "1\n2\n3\nX\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\nY\n20\n",
    "dup1": "x\nx\n", "dup2": "x\n",
}

# (command run in /tmp/d, exact output, exit status)
EXACT = [
    ("diff f1 f2", "2c2\n< b\n---\n> B\n12c12,13\n< l\n---\n> L\n> m\n", 1),
    ("diff --normal f1 f2", "2c2\n< b\n---\n> B\n12c12,13\n< l\n---\n> L\n> m\n", 1),
    ("diff -u " + LAB + "f1 f2",
     "--- A\n+++ B\n@@ -1,5 +1,5 @@\n a\n-b\n+B\n c\n d\n e\n@@ -9,4 +9,5 @@\n i\n j\n k\n-l\n+L\n+m\n", 1),
    ("diff -U1 " + LAB + "f1 f2", "--- A\n+++ B\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n@@ -11,2 +11,3 @@\n k\n-l\n+L\n+m\n", 1),
    ("diff -U 0 " + LAB + "f1 f2", "--- A\n+++ B\n@@ -2 +2 @@\n-b\n+B\n@@ -12 +12,2 @@\n-l\n+L\n+m\n", 1),
    ("diff --unified=1 " + LAB + "f1 f2", "--- A\n+++ B\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n@@ -11,2 +11,3 @@\n k\n-l\n+L\n+m\n", 1),
    ("diff -c " + LAB + "f1 f2",
     "*** A\n--- B\n***************\n*** 1,5 ****\n  a\n! b\n  c\n  d\n  e\n--- 1,5 ----\n  a\n! B\n  c\n  d\n  e\n"
     "***************\n*** 9,12 ****\n  i\n  j\n  k\n! l\n--- 9,13 ----\n  i\n  j\n  k\n! L\n! m\n", 1),
    ("diff -C1 " + LAB + "f1 f2",
     "*** A\n--- B\n***************\n*** 1,3 ****\n  a\n! b\n  c\n--- 1,3 ----\n  a\n! B\n  c\n"
     "***************\n*** 11,12 ****\n  k\n! l\n--- 11,13 ----\n  k\n! L\n! m\n", 1),
    ("diff e f1", "0a1,12\n> a\n> b\n> c\n> d\n> e\n> f\n> g\n> h\n> i\n> j\n> k\n> l\n", 1),
    ("diff -u " + LAB + "e x1", "--- A\n+++ B\n@@ -0,0 +1,3 @@\n+a\n+b\n+c\n", 1),
    ("diff -u " + LAB + "x1 e", "--- A\n+++ B\n@@ -1,3 +0,0 @@\n-a\n-b\n-c\n", 1),
    ("diff -c " + LAB + "e x1", "*** A\n--- B\n***************\n*** 0 ****\n--- 1,3 ----\n+ a\n+ b\n+ c\n", 1),
    ("diff -c " + LAB + "x1 e", "*** A\n--- B\n***************\n*** 1,3 ****\n- a\n- b\n- c\n--- 0 ----\n", 1),
    ("diff x1 x2", "2d1\n< b\n", 1),
    ("diff x2 x1", "1a2\n> b\n", 1),
    ("diff y1 y2", "1,2c1,2\n< a\n< b\n---\n> c\n> d\n", 1),
    ("diff -u " + LAB + "x1 x2", "--- A\n+++ B\n@@ -1,3 +1,2 @@\n a\n-b\n c\n", 1),
    ("diff -u " + LAB + "x2 x1", "--- A\n+++ B\n@@ -1,2 +1,3 @@\n a\n+b\n c\n", 1),
    ("diff -u " + LAB + "dup1 dup2", "--- A\n+++ B\n@@ -1,2 +1 @@\n x\n-x\n", 1),
    ("diff dup1 dup2", "2d1\n< x\n", 1),
    ("diff f1 f1", "", 0),
    ("diff e e", "", 0),
    ("diff -s f1 f1", "Files f1 and f1 are identical\n", 0),
    ("diff -q f1 f2", "Files f1 and f2 differ\n", 1),
    ("diff --brief f1 f1", "", 0),
    ("diff n1 n2", "2c2\n< b\n\\ No newline at end of file\n---\n> b\n", 1),
    ("diff n2 n1", "2c2\n< b\n---\n> b\n\\ No newline at end of file\n", 1),
    ("diff -u " + LAB + "n1 n2", "--- A\n+++ B\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n", 1),
    ("diff -c " + LAB + "n1 n2", "*** A\n--- B\n***************\n*** 1,2 ****\n  a\n! b\n\\ No newline at end of file\n--- 1,2 ----\n  a\n! b\n", 1),
    ("diff n1 n1", "", 0),
    ("diff w1 w2", "1c1\n< Hello  World \n---\n> hello world\n", 1),
    ("diff -i w1 w2", "1c1\n< Hello  World \n---\n> hello world\n", 1),
    ("diff -b w1 w2", "1c1\n< Hello  World \n---\n> hello world\n", 1),
    ("diff -ib w1 w2", "", 0),
    ("diff -iw w1 w2", "", 0),
    ("diff --ignore-case --ignore-space-change w1 w2", "", 0),
    ("diff -w w1 w2", "1c1\n< Hello  World \n---\n> hello world\n", 1),
    ("diff p q", "2d1\n< \n", 1),
    ("diff -B p q", "", 0),
    ("diff -Bu p q", "", 0),
    ("diff -b s1 s2", "1c1\n<  a\n---\n> a\n", 1),
    ("diff -w s1 s2", "", 0),
    ("diff crlf lf | tr '\\r' R; diff -q crlf lf > /dev/null; echo rc=$?", "1,2c1,2\n< aR\n< bR\n---\n> a\n> b\nrc=1\n", 0),
    ("diff --strip-trailing-cr crlf lf", "", 0),
    ("diff -u " + LAB + "g1 g2", "--- A\n+++ B\n@@ -1,7 +1,7 @@\n 1\n 2\n 3\n-4\n+X\n 5\n 6\n 7\n@@ -9,7 +9,7 @@\n 9\n 10\n 11\n-12\n+Y\n 13\n 14\n 15\n", 1),
    ("diff -u " + LAB + "g1 g3",
     "--- A\n+++ B\n@@ -1,7 +1,7 @@\n 1\n 2\n 3\n-4\n+X\n 5\n 6\n 7\n@@ -16,5 +16,5 @@\n 16\n 17\n 18\n-19\n+Y\n 20\n", 1),
    ("diff -U0 " + LAB + "g1 g3", "--- A\n+++ B\n@@ -4 +4 @@\n-4\n+X\n@@ -19 +19 @@\n-19\n+Y\n", 1),
    ("diff g1 g3", "4c4\n< 4\n---\n> X\n19c19\n< 19\n---\n> Y\n", 1),
    ("diff f1 nosuch", "diff: nosuch: No such file or directory\n", 2),
    ("diff nosuch f1", "diff: nosuch: No such file or directory\n", 2),
    ("diff f1", "diff: missing operand after 'f1'\ndiff: Try 'diff --help' for more information.\n", 2),
    ("diff -N e nosuch", "", 0),
    ("diff -N nosuch x1", "0a1,3\n> a\n> b\n> c\n", 1),
    ("cat x1 | diff - x2", "2d1\n< b\n", 1),
    ("cat x2 | diff x1 -", "2d1\n< b\n", 1),
    ("diff -q /tmp/d/f1 /tmp/d/f2 && echo same || echo differ", "Files /tmp/d/f1 and /tmp/d/f2 differ\ndiffer\n", 0),
]

TIME_RE = r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{9} [+-]\d{4}"


def put(vm, path, text):
    esc = text.replace("\\", "\\\\").replace("%", "%%").replace("\n", "\\n").replace("\r", "\\r")
    out, rc = test_shell.run(vm, f"printf '{esc}' > {path}")
    assert rc == 0, out


def setup(vm):
    out, rc = test_shell.run(vm, "rm -rf /tmp/d; mkdir -p /tmp/d && cd /tmp/d")
    for name, text in FILES.items():
        put(vm, "/tmp/d/" + name, text)
    out, rc = test_shell.run(vm, "printf 'a\\0b\\n' > bin1; printf 'a\\0c\\n' > bin2; pwd")
    assert "/tmp/d" in out, out


def check_exact(vm, table, failures):
    for cmd, want, status in table:
        try:
            out, rc = test_shell.run(vm, cmd)
            assert out == want, f"output {out!r}, expected {want!r}"
            assert rc == status, f"exit {rc}, expected {status}; output {out!r}"
        except Exception as e:  # noqa: BLE001
            failures.append(f"{cmd}: {str(e)[-400:]}")


@test
def diff_formats():
    """diff: normal, unified and context output byte for byte, -q -s -a -b -w -i -B, no-newline marker, exit statuses."""
    failures = []
    with Qemu(cmdline="text", name="t-diff-fmt") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        setup(vm)
        check_exact(vm, EXACT, failures)
        # binary files
        for cmd, want, status in [
            ("diff bin1 bin2", "Binary files bin1 and bin2 differ\n", 1),
            ("diff bin1 bin1", "", 0),
            ("diff -q bin1 bin2", "Files bin1 and bin2 differ\n", 1),
        ]:
            out, rc = test_shell.run(vm, cmd)
            if out != want or rc != status:
                failures.append(f"{cmd}: {out!r} rc={rc}")
        out, rc = test_shell.run(vm, "diff -a bin1 bin2 | wc -l")
        if out.strip() != "4":
            failures.append(f"diff -a on binary files: {out!r}")
        # headers carry the file name and a modification time
        out, rc = test_shell.run(vm, "diff -u f1 f2 | head -2")
        import re
        if not re.fullmatch(r"--- f1\t" + TIME_RE + r"\n\+\+\+ f2\t" + TIME_RE + r"\n", out):
            failures.append(f"unified header: {out!r}")
        out, rc = test_shell.run(vm, "diff -c f1 f2 | head -2")
        if not re.fullmatch(r"\*\*\* f1\t" + TIME_RE + r"\n--- f2\t" + TIME_RE + r"\n", out):
            failures.append(f"context header: {out!r}")
        # bad option
        out, rc = test_shell.run(vm, "diff --nonsense f1 f2 2>&1 | head -1; diff -Z f1 f2 >/dev/null 2>&1; echo $?")
        if not out.strip().endswith("2"):
            failures.append(f"bad option status: {out!r}")
    assert not failures, "\n".join(failures)


DIRS = [
    ("diff a b", "Only in a: onlya\nOnly in b: onlyb\nCommon subdirectories: a/sub and b/sub\ndiff a/x b/x\n2c2\n< 2\n---\n> X\n", 1),
    ("diff -r a b",
     "Only in a: onlya\nOnly in b: onlyb\ndiff -r a/sub/s b/sub/s\n1c1\n< q\n---\n> r\ndiff -r a/x b/x\n2c2\n< 2\n---\n> X\n", 1),
    ("diff -rq a b", "Only in a: onlya\nOnly in b: onlyb\nFiles a/sub/s and b/sub/s differ\nFiles a/x and b/x differ\n", 1),
    ("diff -rN a b",
     "diff -rN a/onlya b/onlya\n1d0\n< o\ndiff -rN a/onlyb b/onlyb\n0a1\n> p\ndiff -rN a/sub/s b/sub/s\n1c1\n< q\n---\n> r\n"
     "diff -rN a/x b/x\n2c2\n< 2\n---\n> X\n", 1),
    ("diff -r -x onlya -x 'only*' -x sub a b", "diff -r -x onlya -x 'only*' -x sub a/x b/x\n2c2\n< 2\n---\n> X\n", 1),
    ("diff -rs a/sub b/sub", "diff -rs a/sub/s b/sub/s\n1c1\n< q\n---\n> r\n", 1),
    ("diff -s a/same b/same", "Files a/same and b/same are identical\n", 0),
    ("diff a/x b", "2c2\n< 2\n---\n> X\n", 1),
    ("diff a b/x", "2c2\n< 2\n---\n> X\n", 1),
    ("diff -r c d", "Binary files c/bin and d/bin differ\ndiff -r c/t d/t\n2c2\n< b\n---\n> c\n", 1),
    ("diff -rq c d", "Files c/bin and d/bin differ\nFiles c/t and d/t differ\n", 1),
    ("diff -r e1 e2", "File e1/zz is a directory while file e2/zz is a regular file\n", 1),
    ("diff -r a2 a2", "", 0),
    ("diff -ru --label L1 --label L2 f1 f2 | head -2", "--- L1\n+++ L2\n", None),
    ("diff -r nosuchdir a", "diff: nosuchdir: No such file or directory\n", 2),
]


@test
def diff_dirs():
    """diff -r on directories: Only in, Common subdirectories, headers, -N, -q, -x, file against directory."""
    failures = []
    with Qemu(cmdline="text", name="t-diff-dir") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        setup(vm)
        out, rc = test_shell.run(vm, "mkdir -p a/sub b/sub c d e1/zz e2 a2; printf '1\\n2\\n3\\n' > a/x; printf '1\\nX\\n3\\n' > b/x; "
                                     "echo o > a/onlya; echo p > b/onlyb; echo q > a/sub/s; echo r > b/sub/s; echo same > a/same; echo same > b/same; "
                                     "printf 'a\\0b\\n' > c/bin; printf 'a\\0c\\n' > d/bin; printf 'a\\nb\\n' > c/t; printf 'a\\nc\\n' > d/t; echo hi > e2/zz")
        assert rc == 0, out
        check_exact(vm, [t for t in DIRS if t[2] is not None], failures)
        out, rc = test_shell.run(vm, DIRS[-2][0])
        if not out.startswith("--- L1\n+++ L2\n"):
            failures.append(f"-ru with labels: {out!r}")
        # the diff -r header line echoes the options as typed
        out, rc = test_shell.run(vm, "diff -ru a b | grep '^diff'")
        if out != "diff -ru a/sub/s b/sub/s\ndiff -ru a/x b/x\n":
            failures.append(f"header lines: {out!r}")
        out, rc = test_shell.run(vm, "diff -r -u a b | grep '^diff'")
        if out != "diff -r -u a/sub/s b/sub/s\ndiff -r -u a/x b/x\n":
            failures.append(f"header lines (separate options): {out!r}")
    assert not failures, "\n".join(failures)


PATCH_SETUP = {
    "old": "".join(f"{i}\n" for i in range(1, 21)),
}


@test
def patch_checks():
    """patch: unified, context and normal diffs, -R, --dry-run, -p, offsets, rejects, new and deleted files, no-newline."""
    failures = []

    def chk(vm, cmd, want_sub=None, status=None, exact=None):
        try:
            out, rc = test_shell.run(vm, cmd)
            if want_sub is not None:
                assert want_sub in out, f"expected {want_sub!r} in {out!r}"
            if exact is not None:
                assert out == exact, f"output {out!r}, expected {exact!r}"
            if status is not None:
                assert rc == status, f"exit {rc}, expected {status}; output {out!r}"
        except Exception as e:  # noqa: BLE001
            failures.append(f"{cmd}: {str(e)[-400:]}")

    with Qemu(cmdline="text", name="t-patch") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        setup(vm)
        chk(vm, "diff -u f1 f2 > u.diff; diff -c f1 f2 > c.diff; diff f1 f2 > n.diff; diff -u g1 g3 > g.diff; echo ok", exact="ok\n")
        for fmt in ("u", "c", "n"):
            chk(vm, f"cp f1 t; patch t < {fmt}.diff; cmp t f2 && echo SAME", exact="patching file t\nSAME\n", status=0)
        chk(vm, "cp f1 t; patch -i u.diff t; cmp t f2 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp f1 t; patch t -i u.diff -s; cmp t f2 && echo SAME", exact="SAME\n")
        chk(vm, "cp f2 t; patch -R t < u.diff; cmp t f1 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp f2 t; patch -R t < c.diff; cmp t f1 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp f2 t; patch --reverse t < n.diff; cmp t f1 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp f1 t; patch --dry-run t < u.diff; cmp t f1 && echo UNCHANGED", exact="checking file t\nUNCHANGED\n", status=0)
        chk(vm, "cp f1 t; patch -o out.txt t < u.diff; cmp t f1 && cmp out.txt f2 && echo BOTH", exact="patching file t\nBOTH\n")
        chk(vm, "cp f1 t; patch -b t < u.diff; cmp t.orig f1 && echo ORIG", exact="patching file t\nORIG\n")
        chk(vm, "cp f1 t; patch -b -z .bak t < u.diff; cmp t.bak f1 && echo BAK", exact="patching file t\nBAK\n")
        chk(vm, "rm -rf pd; mkdir pd; cp f1 pd/t; patch -d pd t < u.diff; cmp pd/t f2 && echo SAME", exact="patching file t\nSAME\n")
        # names from the headers: -p1 with a/ b/ prefixes, the file that exists is chosen
        chk(vm, "rm -rf pp; mkdir pp; cp f1 pp/t; sed 's/^--- f1/--- a\\/t/; s/^+++ f2/+++ b\\/t/' u.diff > pp/h.diff; "
                "cd pp; patch -p1 < h.diff; cmp t ../f2 && echo SAME; cd ..", exact="patching file t\nSAME\n")
        chk(vm, "rm -rf pp; mkdir pp; cp f1 pp/f1; cd pp; patch < ../u.diff; cmp f1 ../f2 && echo SAME; cd ..", exact="patching file f1\nSAME\n")
        chk(vm, "rm -rf pp; mkdir pp; cd pp; patch -p0 < ../u.diff; echo rc=$?; cd ..", want_sub="can't find file to patch")
        # offsets
        chk(vm, "printf 'x\\ny\\nz\\n' | cat - g1 > sh; patch sh < g.diff; echo rc=$?",
            exact="patching file sh\nHunk #1 succeeded at 4 (offset 3 lines).\nHunk #2 succeeded at 19 (offset 3 lines).\nrc=0\n")
        chk(vm, "(printf 'p\\nq\\n'; cat g1) > sh; patch sh < g.diff; cmp sh g3 && echo SAME", want_sub="offset 2 lines")
        chk(vm, "(printf 'p\\n'; cat g1) > sh; patch sh < g.diff", want_sub="(offset 1 line).", status=0)
        chk(vm, "sed 8,9d g1 > sh; patch sh < g.diff", exact="patching file sh\nHunk #2 succeeded at 14 (offset -2 lines).\n", status=0)
        # failure: the second hunk does not match; a reject file is written
        chk(vm, "sed 's/^19$/NINETEEN/' g1 > bad; patch bad < g.diff; echo rc=$?",
            exact="patching file bad\nHunk #2 FAILED at 16.\n1 out of 2 hunks FAILED -- saving rejects to file bad.rej\nrc=1\n")
        chk(vm, "sed 's/^19$/NINETEEN/' g1 > bad; patch bad < g.diff >/dev/null; echo rc=$?; grep -c '^@@' bad.rej; sed -n 4p bad", exact="rc=1\n1\nX\n")
        chk(vm, "cp f1 t; patch -r my.rej t < u.diff; echo rc=$?; test -f my.rej && echo REJ || echo NOREJ", exact="patching file t\nrc=0\nNOREJ\n")
        chk(vm, "printf 'zz\\n' > t; patch t < u.diff; echo rc=$?", want_sub="rc=1")
        chk(vm, "printf 'zz\\n' > t; patch t < u.diff; test -f t.rej && echo REJ", want_sub="REJ")
        # no input / garbage / missing file
        chk(vm, "echo hello | patch t; echo rc=$?", want_sub="rc=2")
        chk(vm, "patch nosuchfile < u.diff; echo rc=$?", want_sub="rc=2")
        chk(vm, "patch -i nosuch.diff; echo rc=$?", want_sub="rc=2")
        chk(vm, "patch --bogus < u.diff 2>/dev/null; echo rc=$?", want_sub="rc=2")
        # creating and deleting files
        chk(vm, "rm -rf nf; mkdir nf; diff -u /dev/null f1 > nf/new.diff; cd nf; patch < new.diff; cd ..; ", want_sub="patching file")
        chk(vm, "rm -rf nf; mkdir nf; diff -u e f1 | sed 's/^--- e/--- \\/dev\\/null/; s/^+++ f1/+++ made\\/f/' > nf/new.diff; "
                "cd nf; patch < new.diff; cmp made/f ../f1 && echo CREATED; cd ..", want_sub="CREATED")
        chk(vm, "rm -rf nf; mkdir nf; cp f1 nf/gone; diff -u f1 e | sed 's/^--- f1/--- gone/; s/^+++ e/+++ \\/dev\\/null/' > nf/del.diff; "
                "cd nf; patch < del.diff; test -f gone && echo STILL || echo DELETED; cd ..", want_sub="DELETED")
        chk(vm, "cp f1 t; diff -u f1 e | patch -E t; test -f t && echo STILL || echo DELETED", want_sub="DELETED")
        chk(vm, "cp f1 t; diff -u f1 e | patch t; test -s t && echo STILL || echo EMPTY", want_sub="EMPTY")
        # several files in one patch
        chk(vm, "rm -rf mf; mkdir mf; cp f1 mf/one; cp g1 mf/two; (diff -u f1 f2 | sed 's/^--- f1.*/--- one/; s/^+++ f2.*/+++ one/'; "
                "diff -u g1 g3 | sed 's/^--- g1.*/--- two/; s/^+++ g3.*/+++ two/') > mf/both.diff; cd mf; patch < both.diff; "
                "cmp one ../f2 && cmp two ../g3 && echo BOTHOK; cd ..", exact="patching file one\npatching file two\nBOTHOK\n")
        # diff -r output is understood: the "diff -r" lines are skipped
        chk(vm, "rm -rf rr; mkdir -p rr/a1/sub rr/a2/sub; printf '1\\n2\\n3\\n' > rr/a1/x; printf '1\\nX\\n3\\n' > rr/a2/x; echo q > rr/a1/sub/s; echo r > rr/a2/sub/s; "
                "(cd rr; diff -ru a1 a2 > ../r.diff); cd rr; patch -p1 -d a1 < ../r.diff > /dev/null; cd ..; cmp rr/a1/x rr/a2/x && cmp rr/a1/sub/s rr/a2/sub/s && echo RSAME",
            want_sub="RSAME")
        # missing newline at the end of a file
        chk(vm, "cp n1 t; diff -u n1 n2 > nn.diff; patch t < nn.diff; cmp t n2 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp n2 t; diff -u n2 n1 > nn.diff; patch t < nn.diff; cmp t n1 && echo SAME", exact="patching file t\nSAME\n")
        chk(vm, "cp n1 t; patch -R t < nn.diff; cmp t n2 && echo SAME", want_sub="SAME")
        # CRLF content survives
        chk(vm, "printf 'a\\r\\nb\\r\\nc\\r\\n' > cr1; printf 'a\\r\\nB\\r\\nc\\r\\n' > cr2; diff -u cr1 cr2 > cr.diff; cp cr1 t; patch t < cr.diff; cmp t cr2 && echo SAME",
            exact="patching file t\nSAME\n")
        # reversed patch is detected, -N skips it
        chk(vm, "cp f2 t; patch -N t < u.diff; echo rc=$?; cmp t f2 && echo UNTOUCHED", want_sub="rc=1")
    assert not failures, "\n".join(failures)


def random_text(rng, vocab, nlines):
    return "".join(rng.choice(vocab) + "\n" for _ in range(nlines))


def mutate(rng, text, vocab):
    lines = text.splitlines(keepends=True)
    for _ in range(rng.randint(0, 6)):
        kind = rng.randint(0, 3)
        if kind == 0 and lines:
            i = rng.randrange(len(lines))
            del lines[i:i + rng.randint(1, 3)]
        elif kind == 1:
            i = rng.randint(0, len(lines))
            lines[i:i] = [rng.choice(vocab) + "\n" for _ in range(rng.randint(1, 3))]
        elif kind == 2 and lines:
            i = rng.randrange(len(lines))
            lines[i] = rng.choice(vocab) + "\n"
        elif kind == 3 and len(lines) > 3:
            i = rng.randrange(len(lines) - 2)
            lines[i], lines[i + 2] = lines[i + 2], lines[i]
    out = "".join(lines)
    if out and rng.random() < 0.1:
        out = out[:-1]                              # no newline at the end
    return out


@test
def diff_roundtrip():
    """diff | patch round trips on random files (unified, context, normal, reverse); the two diffs agree on 'differ'."""
    rng = random.Random(20260101)
    failures = []
    with Qemu(cmdline="text", name="t-diff-rt") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = test_shell.run(vm, "rm -rf /tmp/rt; mkdir -p /tmp/rt && cd /tmp/rt && pwd")
        for case in range(28):
            vocab = rng.choice([["a", "b", "c"], ["x", "y", "z", "w", "v", "u", "t", ""], [str(i) for i in range(30)], ["", "}", "{", "int x;", "return;"]])
            a = random_text(rng, vocab, rng.choice([0, 1, 2, 5, 12, 30, 60]))
            b = mutate(rng, a, vocab) if rng.random() < 0.85 else random_text(rng, vocab, rng.randint(0, 40))
            put(vm, "/tmp/rt/a", a)
            put(vm, "/tmp/rt/b", b)
            for fmt, reverse in (("-u", False), ("-c", False), ("", False), ("-u", True), ("-c", True)):
                cmd = (f"cp b w; diff {fmt} b a > p; patch -s w < p; cmp -s w a && echo OK" if reverse else
                       f"cp a w; diff {fmt} a b > p; patch -s w < p; cmp -s w b && echo OK")
                rc_expect = 0 if a == b else 1
                out, rc = test_shell.run(vm, cmd + "; diff a b > /dev/null; echo diffrc=$?")
                if "OK" not in out or f"diffrc={rc_expect}" not in out:
                    failures.append(f"case {case} fmt {fmt!r} reverse={reverse}: {out!r}\n  a={a!r}\n  b={b!r}")
                    break
            out, rc = test_shell.run(vm, "diff -u a b > p; cp a w; patch -R -s w < p; echo rc=$?; cmp -s w a && echo UNCH || echo NEEDS")
        # a patch that was already applied is refused with -R's absence: reverse then forward
    assert not failures, "\n".join(failures[:6])


@test
def diff_big():
    """diff / patch on bigger inputs: a few changes in a large file, two unrelated 20000-line files, a 100 KB line."""
    failures = []
    with Qemu(cmdline="text", name="t-diff-big") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = test_shell.run(vm, "rm -rf /tmp/big; mkdir -p /tmp/big && cd /tmp/big && seq 1 150000 > b1 && "
                                     "sed '75s/.*/seventy five/;80000s/.*/EIGHTY K/;149999d' b1 > b2 && wc -l b1 b2", timeout=300)
        out, rc = test_shell.run(vm, "diff b1 b2", timeout=300)
        want = "75c75\n< 75\n---\n> seventy five\n80000c80000\n< 80000\n---\n> EIGHTY K\n149999d149998\n< 149999\n"
        if out != want or rc != 1:
            failures.append(f"big diff: {out!r} rc={rc}")
        out, rc = test_shell.run(vm, "diff -u b1 b2 > big.diff; cp b1 w; patch -s w < big.diff; cmp w b2 && echo ROUNDTRIP; wc -l < big.diff", timeout=300)
        if "ROUNDTRIP" not in out:
            failures.append(f"big patch: {out!r}")
        out, rc = test_shell.run(vm, "seq 1 20000 > u1; seq 20001 40000 > u2; diff u1 u2 | wc -l; diff -q u1 u2", timeout=300)
        if not out.startswith("40002\n") or "differ" not in out:
            failures.append(f"unrelated files: {out!r}")
        # shuffled lines: every line matches something, so the real search runs; the result must still be a valid diff
        out, rc = test_shell.run(vm, "seq 1 3000 > s1; shuf s1 > s2; diff -u s1 s2 > s.diff; cp s1 w; patch -s w < s.diff; cmp w s2 && echo VALID", timeout=300)
        if "VALID" not in out:
            failures.append(f"shuffled: {out!r}")
        out, rc = test_shell.run(vm, "seq 1 20000 | tr '\\n' ' ' > ln1; echo end >> ln1; sed 's/end/END/' ln1 > ln2; diff ln1 ln2 | wc -l", timeout=300)
        if out.strip() != "4":
            failures.append(f"long line: {out!r}")
    assert not failures, "\n".join(failures)
