"""awk: the POSIX awk applet. Each check types one command into gapsh over the serial console (see test_shell.py).

An expected string starting with "=" must equal the whole output (trailing newline ignored); any other string only has
to appear in the output. The exit status is checked when it is not None.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu

TABLE = [
    # --- fields, print, variables
    ("echo 'a b c' | awk '{print $2}'", "=b", 0),
    ("echo 'a b c' | awk '{print NF, $NF, $(NF-1)}'", "=3 c b", 0),
    ("echo '  a   b  ' | awk '{print NF}'", "=2", 0),
    ("echo 'a b' | awk 'BEGIN{OFS=\"-\"}{print $1,$2}'", "=a-b", 0),
    ("echo 'a b c' | awk '{$2=\"X\"; print; print NF}'", "=a X c\n3", 0),
    ("echo 'a b c d' | awk '{NF=2; print}'", "=a b", 0),
    ("echo 'a b c' | awk 'BEGIN{OFS=\"-\"}{NF=5; print}'", "=a-b-c--", 0),
    ("echo 'a b' | awk '{$0=\"x y z\"; print NF, $3}'", "=3 z", 0),
    ("echo a | awk '{$3=\"c\"; print; print NF}'", "=a  c\n3", 0),
    ("echo 'a   b' | awk -v OFS=, '{$1=$1}1'", "=a,b", 0),
    ("echo 'a:b:c' | awk -F: '{print $2}'", "=b", 0),
    ("printf 'a\\tb c\\td\\n' | awk -F'\\t' '{print $2}'", "=b c", 0),
    ("printf 'a\\tb c\\td\\n' | awk -Ft '{print $3}'", "=d", 0),
    ("echo 'a1b22c' | awk -F'[0-9]+' '{print $1 $2 $3}'", "=abc", 0),
    ("echo 'a,,b' | awk -F, '{print NF}'", "=3", 0),
    ("echo 'a.b' | awk -F. '{print $2}'", "=b", 0),
    ("echo 'a|b' | awk -F'|' '{print $2}'", "=b", 0),
    ("echo 'a:b' | awk -v FS=: '{print $2}'", "=b", 0),
    ("awk -v x=5 'BEGIN{print x+1}'", "=6", 0),
    ("awk -v 's=a\\tb' 'BEGIN{print s}'", "=a\tb", 0),
    ("echo a | awk '{print x}' x=7 -", "=7", 0),
    ("printf 'a\\nb\\n' | awk 'END{print NR, $0}'", "=2 b", 0),
    ("awk 'BEGIN{print \"hi\", \"there\"}'", "=hi there", 0),
    ("awk 'BEGIN{print(\"a\",\"b\"); printf(\"%s-%s\\n\", \"a\", \"b\")}'", "=a b\na-b", 0),
    ("awk -- 'BEGIN{print \"dd\"}'", "=dd", 0),
    ("awk --version", "awk version", 0),
    ("printf 'BEGIN{print \"from file\"}\\n' > /tmp/p.awk; awk -f /tmp/p.awk", "=from file", 0),
    ("printf '# c\\nBEGIN {\\n x = 1 + \\\\\\n 2\\n if (x == 3)\\n  print \"three\"\\n else\\n  print \"other\"\\n}\\n' > /tmp/p2.awk; awk -f /tmp/p2.awk", "=three", 0),
    # --- patterns
    ("printf 'foo\\nbar\\nfoobar\\n' | awk '/foo/'", "=foo\nfoobar", 0),
    ("printf 'foo\\nbar\\nfoobar\\n' | awk '!/foo/'", "=bar", 0),
    ("seq 1 10 | awk 'NR==3,NR==5'", "=3\n4\n5", 0),
    ("seq 1 5 | awk 'NR%2{next}{print}'", "=2\n4", 0),
    ("seq 1 6 | awk '$1>2 && $1<5'", "=3\n4", 0),
    ("echo 'foo bar' | awk '$2 ~ /^b/ {print \"y\"}'", "=y", 0),
    ("printf 'a\\nb\\n' | awk 'BEGIN{print \"s\"} {n++} END{print n}'", "=s\n2", 0),
    # --- numbers, strings, comparisons
    ("awk 'BEGIN{print 1==1.0, \"a\"<\"b\", 2<10, \"2\"<\"10\"}'", "=1 1 1 0", 0),
    ("echo '10 9' | awk '{print ($1 > $2)}'", "=1", 0),
    ("echo '10 9x' | awk '{print ($1 > $2)}'", "=0", 0),
    ("awk 'BEGIN{x=\"10\"; y=9; print (x > y)}'", "=0", 0),
    ("awk 'BEGIN{if (x == 0 && x == \"\") print \"ok\"}'", "=ok", 0),
    ("awk 'BEGIN{print x+0, \"[\" x \"]\", length(x)}'", "=0 [] 0", 0),
    ("awk 'BEGIN{print 1e6, 100000*100000, 0.1+0.2, 1/3}'", "=1000000 10000000000 0.3 0.333333", 0),
    ("awk 'BEGIN{CONVFMT=\"%.2g\"; a=3.14159; b=a \"\"; print b}'", "=3.1", 0),
    ("awk 'BEGIN{OFMT=\"%.2f\"; print 3.14159, 3.14159 \"\"}'", "=3.14 3.14159", 0),
    ("awk 'BEGIN{print 1 \" \" 2+3 \" \" 4}'", "=1 5 4", 0),
    ("awk 'BEGIN{print -2^2, 2^3^2, 2**3}'", "=-4 512 8", 0),
    ("awk 'BEGIN{print 7%3, -7%3, int(-3.7), int(\"12abc\")}'", "=1 -1 -3 12", 0),
    ("awk 'BEGIN{x=5; print (x>3 ? \"big\" : \"small\"), (x>3 && x<4), (x<3 || x>4), !x}'", "=big 0 1 0", 0),
    ("awk 'BEGIN{i=5; print i++, i, ++i, i--, --i}'", "=5 6 7 7 5", 0),
    ("awk 'BEGIN{x=10; x+=5; x-=3; x*=2; x/=4; x%=4; x^=2; print x}'", "=4", 0),
    ("awk 'BEGIN{print -\"3\", +\"4x\", !\"a\", !\"\"}'", "=-3 4 0 1", 0),
    ("awk 'BEGIN{print (\"abc\" < \"abd\"), (10 < 9), (\"10\" < \"9\")}'", "=1 0 1", 0),
    ("awk 'BEGIN{print \" 12 \"+1, \"1e2\"+0, \".5\"+0, \"+3\"+0, \"0x1A\"+0}'", "=13 100 0.5 3 0", 0),
    ("awk 'BEGIN{print 2^53, 2^53+1, 123456789012}'", "=9007199254740992 9007199254740992 123456789012", 0),
    # --- control flow
    ("awk 'BEGIN{for(i=0;i<10;i++){if(i==2)continue; if(i==5)break; s=s i}; print s; do {j++} while (j<3); print j; while (k<4) k++; print k}'", "=0134\n3\n4", 0),
    # --- arrays
    ("awk 'BEGIN{a[1,2]=3; for (k in a) {split(k, p, SUBSEP); print p[1], p[2]}; print ((1,2) in a), ((2,1) in a)}'", "=1 2\n1 0", 0),
    ("awk 'BEGIN{a[1];a[2];delete a[1]; print length(a); delete a; print length(a)}'", "=1\n0", 0),
    ("awk 'BEGIN{a[\"x\"]=1; print (\"x\" in a), (\"y\" in a), length(a)}'", "=1 0 1", 0),
    ("awk 'BEGIN{if (a[\"x\"] == \"\") print length(a)}'", "=1", 0),
    ("printf 'a b a\\nb a\\n' | awk '{for(i=1;i<=NF;i++) c[$i]++} END{print c[\"a\"], c[\"b\"]}'", "=3 2", 0),
    ("awk 'BEGIN{n=split(\"5 3 9 1 7\", a, \" \"); for(i=2;i<=n;i++){v=a[i]; j=i-1; while(j>0 && a[j]>v){a[j+1]=a[j]; j--}; a[j+1]=v}; for(i=1;i<=n;i++) printf \"%s \", a[i]; print \"\"}'", "1 3 5 7 9", 0),
    # --- functions
    ("awk 'function f(n){return n<2?n:f(n-1)+f(n-2)} BEGIN{print f(15)}'", "=610", 0),
    ("awk 'function fill(a){a[1]=\"x\"} BEGIN{fill(arr); print arr[1]}'", "=x", 0),
    ("awk 'function g(a,  t){t=a; a=9; return t} BEGIN{x=3; print g(x), x}'", "=3 3", 0),
    ("awk 'function f(n){return n==0?0:1+f(n-1)} BEGIN{print f(2000)}'", "=2000", 0),
    ("awk 'function f(n){return n==0?0:1+f(n-1)} BEGIN{print f(5000)}'", "=5000", 0),
    # --- printf
    ("awk 'BEGIN{printf \"%5.2f|%-5d|%05d|%x|%o|%c|%c|%s|%e\\n\", 3.14159, 42, 42, 255, 8, 65, \"hello\", \"str\", 12345.678}'", "= 3.14|42   |00042|ff|10|A|h|str|1.234568e+04", 0),
    ("awk 'BEGIN{printf \"%*d|%-*d|\\n\", 5, 42, 4, 7}'", "=   42|7   |", 0),
    ("awk 'BEGIN{printf \"100%%\\n\"}'", "=100%", 0),
    ("awk 'BEGIN{printf \"%i|%5s|%-5s|%.2s|\\n\", 7, \"ab\", \"cd\", \"xyz\"}'", "=7|   ab|cd   |xy|", 0),
    ("awk 'BEGIN{printf \"%u %X %E %G\\n\", 5, 255, 1.5, 0.0001}'", "=5 FF 1.500000E+00 0.0001", 0),
    ("awk 'BEGIN{s=sprintf(\"%03d-%s\", 7, \"x\"); print s}'", "=007-x", 0),
    # --- string functions
    ("awk 'BEGIN{print length(\"hello\"), length(), length}'", "=5 0 0", 0),
    ("awk 'BEGIN{print substr(\"hello\",2,3), substr(\"hello\",0,2), substr(\"hello\",4), substr(\"hello\",-1,3), \"[\" substr(\"hello\",9) \"]\"}'", "=ell h lo h []", 0),
    ("awk 'BEGIN{print index(\"hello\",\"ll\"), index(\"hello\",\"z\")}'", "=3 0", 0),
    ("awk 'BEGIN{n=split(\"a:b:c\", p, \":\"); print n, p[1], p[3]; n=split(\"a b  c\", q); print n, q[3]; n=split(\"abc\", r, \"\"); print n, r[2]}'", "=3 a c\n3 c\n3 b", 0),
    ("awk 'BEGIN{n=split(\"a1b22c\", p, /[0-9]+/); print n, p[3]}'", "=3 c", 0),
    ("awk 'BEGIN{s=\"hello\"; sub(/l/,\"L\",s); print s; t=\"hello\"; n=gsub(/l/,\"[&]\",t); print n, t; u=\"a.b\"; gsub(/\\./,\"\\\\&\",u); print u}'", "=heLlo\n2 he[l][l]o\na&b", 0),
    ("awk 'BEGIN{s=\"abc\"; gsub(/x*/,\"-\",s); print s; t=\"abc\"; gsub(/b*/,\"X\",t); print t}'", "=-a-b-c-\nXaXcX", 0),
    ("echo 'a/b' | awk '{gsub(/[/]/,\"-\"); print}'", "=a-b", 0),
    ("echo 'hello world' | awk '{gsub(/o/,\"0\"); print; print $2}'", "=hell0 w0rld\nw0rld", 0),
    ("awk 'BEGIN{print match(\"foobar\", /o+/), RSTART, RLENGTH; print match(\"x\", /y/), RSTART, RLENGTH}'", "=2 2 2\n0 0 -1", 0),
    ("awk 'BEGIN{print tolower(\"ABC\"), toupper(\"abc\")}'", "=abc ABC", 0),
    ("echo abc | awk '{r=\"^a.c$\"; print ($0 ~ r), ($0 ~ \"x\")}'", "=1 0", 0),
    ("echo 'ab12' | awk '/^[a-z]+[0-9]{2}$/{print \"m\"}'", "=m", 0),
    ("echo 'a/b' | awk '/a\\/b/{print \"slash\"}'", "=slash", 0),
    ("echo 'a.b' | awk '{print ($0 ~ \"a\\\\.b\"), ($0 ~ \"a.b\")}'", "=1 1", 0),
    # --- math
    ("awk 'BEGIN{printf \"%.4f %.4f %.4f %.4f %.4f\\n\", sin(1), cos(1), atan2(1,1)*4, exp(1), log(10)}'", "=0.8415 0.5403 3.1416 2.7183 2.3026", 0),
    ("awk 'BEGIN{print sqrt(16), int(7.9)}'", "=4 7", 0),
    ("awk 'BEGIN{srand(1); a=rand(); srand(1); b=rand(); print (a==b), (a>=0 && a<1)}'", "=1 1", 0),
    ("awk 'BEGIN{print srand(5), srand()}'", "=0 5", 0),
    # --- records
    ("printf 'a;b;c' | awk 'BEGIN{RS=\";\"}{print NR \":\" $0}'", "=1:a\n2:b\n3:c", 0),
    ("printf 'a b\\nc\\n\\n\\nd e\\n' | awk 'BEGIN{RS=\"\"}{print NR \": \" $1 \"-\" $NF \" \" NF}'", "=1: a-c 3\n2: d-e 2", 0),
    ("printf 'a12b3c' | awk 'BEGIN{RS=\"[0-9]+\"}{print}'", "=a\nb\nc", 0),
    ("printf 'a\\nb\\n' | awk 'BEGIN{ORS=\"|\"}{print}END{printf \"\\n\"}'", "=a|b|", 0),
    ("printf '1\\n2\\n' > /tmp/f1; printf '3\\n' > /tmp/f2; awk '{print FILENAME, NR, FNR}' /tmp/f1 /tmp/f2", "=/tmp/f1 1 1\n/tmp/f1 2 2\n/tmp/f2 3 1", 0),
    ("awk 'FNR==2{nextfile}{print}' /tmp/f1 /tmp/f2", "=1\n3", 0),
    ("awk 'BEGIN{print ARGC, ARGV[0], ARGV[1], ARGV[2]}' a b", "=3 awk a b", 0),
    ("export FOO=bar; awk 'BEGIN{print ENVIRON[\"FOO\"]}'", "=bar", 0),
    # --- exit
    ("echo x | awk '{exit 3} END{print \"end\"}'", "=end", 3),
    ("awk 'BEGIN{exit 4}'", "", 4),
    ("awk 'function d(){exit 6} BEGIN{d(); print \"no\"}'", "", 6),
    # --- getline
    ("printf '1\\n2\\n3\\n' > /tmp/g.txt; awk 'BEGIN{while ((getline l < \"/tmp/g.txt\") > 0) s+=l; print s}'", "=6", 0),
    ("awk 'BEGIN{\"echo hi\" | getline x; print x}'", "=hi", 0),
    ("awk 'BEGIN{\"echo a b\" | getline; print $2, NR}'", "=b 1", 0),
    ("printf 'a\\nb\\nc\\n' | awk 'NR==1{getline; print $0, NR}'", "=b 2", 0),
    ("printf 'a\\nb\\n' | awk 'NR==1{getline x; print x, $0, NR}'", "=b a 2", 0),
    ("awk 'BEGIN{print (getline x < \"/nonexistent\")}'", "=-1", 0),
    ("printf 'a b c\\n' > /tmp/g1; awk 'BEGIN{getline < \"/tmp/g1\"; print NF, $2}'", "=3 b", 0),
    ("awk 'BEGIN{while ((\"echo x; echo y\" | getline l) > 0) n++; print n}'", "=2", 0),
    # --- redirection, pipes, system
    ("awk 'BEGIN{print \"a\" > \"/tmp/o1.txt\"; print \"b\" > \"/tmp/o1.txt\"; close(\"/tmp/o1.txt\"); while ((getline l < \"/tmp/o1.txt\") > 0) print \"got\", l}'", "=got a\ngot b", 0),
    ("awk 'BEGIN{print \"c\" >> \"/tmp/o1.txt\"}'; cat /tmp/o1.txt", "=a\nb\nc", 0),
    ("awk 'BEGIN{print \"b\\na\" | \"sort\"; close(\"sort\"); print \"done\"}'", "=a\nb\ndone", 0),
    ("awk 'BEGIN{r = system(\"exit 3\"); print r}'", "=3", 0),
    ("awk 'BEGIN{print \"x\" | \"cat >/dev/null; exit 3\"; print close(\"cat >/dev/null; exit 3\")}'", "=3", 0),
    ("awk 'BEGIN{printf \"x%d\\n\", 5 > \"/dev/stderr\"}' 2>&1", "=x5", 0),
    ("echo 'x y' | awk '{print $1, $2 > \"/dev/stderr\"}' 2>&1", "=x y", 0),
    # --- errors
    ("awk 'BEGIN{print 1/0}'", "division by zero", 2),
    ("awk 'BEGIN{print (}'", "syntax error", 2),
    ("awk '{print}' /nonexistent", "can't open", 2),
    ("awk '{print}' /nonexistent /tmp/f2", "3", 2),
    ("awk", "usage", 2),
    ("awk -f /nonexistent", "can't open", 2),
    ("awk 'BEGIN{x[1]=1; print x}'", "array", 2),
    ("awk 'BEGIN{f(1)}'", "undefined function", 2),
]


def check(vm, table):
    failures = []
    for cmd, expect, status in table:
        try:
            out, rc = test_shell.run(vm, cmd, timeout=90)
            out = out.replace("\r", "")
            if expect.startswith("="):
                assert out.rstrip("\n") == expect[1:], f"expected {expect[1:]!r}, got {out!r}"
            else:
                assert expect in out, f"expected {expect!r} in {out!r}"
            assert status is None or rc == status, f"exit {rc}, expected {status}; output {out!r}"
        except Exception as e:  # noqa: BLE001
            failures.append(f"{cmd}: {str(e)[-300:]}")
    return failures


@test
def awk_checks():
    """awk: fields, patterns, expressions, arrays, functions, printf, builtins, getline, redirection, errors."""
    with Qemu(cmdline="text", name="t-awk") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        failures = check(vm, TABLE)
    assert not failures, "\n".join(failures)


@test
def awk_big_input():
    """awk: a 2 MB generated file through sum, field print and a counting array (performance)."""
    table = [
        ("seq 1 300000 > /tmp/big; ls -l /tmp/big | wc -l", "1", 0),
        ("awk '{s+=$1} END{print s}' /tmp/big", "=45000150000", 0),
        ("awk '{print $1}' /tmp/big | tail -1", "=300000", 0),
        ("awk '{a[$1%10]++} END{print a[0], a[7], length(a)}' /tmp/big", "=30000 30000 10", 0),
        ("awk -F, '{a[$1%100]++} END{n=0; for(k in a) n++; print n}' /tmp/big", "=100", 0),
        ("awk 'END{print NR}' /tmp/big", "=300000", 0),
        ("awk '/99999/{n++} END{print n}' /tmp/big", "=3", 0),
    ]
    with Qemu(cmdline="text", name="t-awk-big") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        failures = []
        for cmd, expect, status in table:
            try:
                out, rc = test_shell.run(vm, cmd, timeout=600)
                out = out.replace("\r", "")
                if expect.startswith("="):
                    assert out.rstrip("\n") == expect[1:], f"expected {expect[1:]!r}, got {out!r}"
                else:
                    assert expect in out, f"expected {expect!r} in {out!r}"
                assert status is None or rc == status, f"exit {rc}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}: {str(e)[-300:]}")
    assert not failures, "\n".join(failures)
