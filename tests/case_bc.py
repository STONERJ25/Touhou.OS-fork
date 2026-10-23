"""bc and dc: arbitrary precision arithmetic, the bc language, the dc stack machine.

The guest shell is driven over the serial console like in test_shell.py.  Expected values for the big numbers are computed
here with Python integers, so the table shows what is being checked.
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu


def wrap(s, width=70):
    """GNU bc/dc line wrapping: width-1 characters and a backslash on every line but the last."""
    out = []
    while len(s) > width - 1:
        out.append(s[:width - 1] + "\\")
        s = s[width - 1:]
    out.append(s)
    return "\n".join(out)


def exact(text):
    return ("==", text)


def raw(text):
    """The output without the final newline (n and P do not print one)."""
    return ("raw", text)


def bc(prog, flags="", env=""):
    """A shell command that feeds PROG (one line, no single quotes) to bc."""
    return f"printf '%s\\n' '{prog}' | {env}bc {flags}".replace("bc  ", "bc ").rstrip()


def prog(*lines):
    """A shell command that writes the lines to /tmp/p.bc and runs bc on that file."""
    body = "\\n".join(lines)
    return f"printf '{body}\\n' > /tmp/p.bc; bc /tmp/p.bc"


def frac_digits(a, b, scale):
    """a/b truncated to SCALE digits, written the way bc prints it."""
    q = str(a * 10 ** scale // b).rjust(scale + 1, "0")
    return q[:-scale] + "." + q[-scale:]


F100 = str(math.factorial(100))
F1000 = str(math.factorial(1000))

# (command typed into gapsh, expected output, expected exit status or None)
# expected is a substring of the output, or exact("...") for the whole output
BC = [
    # --- integers and the four operations
    (bc("1+2"), exact("3"), 0),
    (bc("7-10"), exact("-3"), 0),
    (bc("6*7"), exact("42"), 0),
    (bc("2^100"), exact("1267650600228229401496703205376"), 0),
    (bc("2^200"), exact(str(2 ** 200)), 0),
    (bc("123456789012345678901234567890 * 987654321098765432109876543210"),
     exact(str(123456789012345678901234567890 * 987654321098765432109876543210)), 0),
    (bc("1000000000 * 1000000000"), exact("1000000000000000000"), 0),
    (bc("999999999 + 1"), exact("1000000000"), 0),
    (bc("1000000000 - 1"), exact("999999999"), 0),
    (bc("10^30 - 1"), exact("9" * 30), 0),
    (bc("-5 + 3"), exact("-2"), 0),
    (bc("3 - -3"), exact("6"), 0),
    (bc("(1+2)*(3+4)"), exact("21"), 0),
    (bc("2^3^2"), exact("512"), 0),
    (bc("2*3+4*5"), exact("26"), 0),
    (bc("2^0"), exact("1"), 0),
    (bc("0^0"), exact("1"), 0),
    (bc("7%3"), exact("1"), 0),
    (bc("-7%3"), exact("-1"), 0),
    (bc("7%-3"), exact("1"), 0),
    (bc("10/3"), exact("3"), 0),
    (bc("-7/2"), exact("-3"), 0),
    (bc("0*5"), exact("0"), 0),
    # --- scale
    (bc("scale=2; 10/3"), exact("3.33"), 0),
    (bc("scale=0; 7/2"), exact("3"), 0),
    (bc("scale=20; 1/3"), exact(".33333333333333333333"), 0),
    (bc("scale=3; 1/3 + 1/3"), exact(".666"), 0),
    (bc("scale=5; 22/7"), exact("3.14285"), 0),
    (bc("scale=2; -10/3"), exact("-3.33"), 0),
    (bc("scale=2; 1.5 * 1.5"), exact("2.25"), 0),
    (bc("scale=1; 1.55 * 1.55"), exact("2.40"), 0),
    (bc("1.50 + 1.5"), exact("3.00"), 0),
    (bc("scale=3; 2.5 - 2.5"), exact("0"), 0),
    (bc("scale=2; 0.5"), exact(".5"), 0),
    (bc("scale=2; -0.5 + 0"), exact("-.5"), 0),
    (bc("scale(1.234)"), exact("3"), 0),
    (bc("scale=7; scale"), exact("7"), 0),
    (bc("length(12345)"), exact("5"), 0),
    (bc("length(1.25)"), exact("3"), 0),
    (bc("scale=2; 5 % 3"), exact(".02"), 0),
    (bc("scale=20; 2^-2"), exact(".25000000000000000000"), 0),
    (bc("scale=3; 2^-3"), exact(".125"), 0),
    (bc("2^-1"), exact("0"), 0),
    (bc("scale=2; 1.5^2"), exact("2.25"), 0),
    (bc("scale=2; 1.5^3"), exact("3.37"), 0),
    (bc("scale=20; sqrt(2)"), exact("1.41421356237309504880"), 0),
    (bc("scale=5; sqrt(2)"), exact("1.41421"), 0),
    (bc("sqrt(16)"), exact("4"), 0),
    (bc("sqrt(10^40)"), exact("1" + "0" * 20), 0),
    (bc("sqrt(0)"), exact("0"), 0),
    (bc("scale=30; sqrt(3)"), exact("1.732050807568877293527446341505"), 0),
    # --- variables, operators, statements
    (bc("a=5; a*2"), exact("10"), 0),
    (bc("a=5"), exact(""), 0),
    (bc("a=b=4; a+b"), exact("8"), 0),
    (bc("(a=5)"), exact("5"), 0),
    (bc("x=3; x++; x"), exact("3\n4"), 0),
    (bc("x=3; ++x"), exact("4"), 0),
    (bc("x=3; x--; x"), exact("3\n2"), 0),
    (bc("x=10; x+=5; x"), exact("15"), 0),
    (bc("x=10; x-=4; x*=3; x"), exact("18"), 0),
    (bc("x=17; x/=5; x"), exact("3"), 0),
    (bc("x=17; x%=5; x"), exact("2"), 0),
    (bc("x=3; x^=3; x"), exact("27"), 0),
    (bc("3>2"), exact("1"), 0),
    (bc("3<2"), exact("0"), 0),
    (bc("3>=3"), exact("1"), 0),
    (bc("2<=1"), exact("0"), 0),
    (bc("3==3"), exact("1"), 0),
    (bc("3!=3"), exact("0"), 0),
    (bc("1&&0"), exact("0"), 0),
    (bc("1&&2"), exact("1"), 0),
    (bc("0||0"), exact("0"), 0),
    (bc("0||3"), exact("1"), 0),
    (bc("!0"), exact("1"), 0),
    (bc("!5"), exact("0"), 0),
    (bc("5; last*2"), exact("5\n10"), 0),
    (bc("5; .+1"), exact("5\n6"), 0),
    (bc("for(i=0;i<3;i++) i"), exact("0\n1\n2"), 0),
    (bc("i=0; while(i<3){ i; i+=1 }"), exact("0\n1\n2"), 0),
    (bc("for(i=0;i<10;i++){ if(i==3) break; i }"), exact("0\n1\n2"), 0),
    (bc("for(i=0;i<5;i++){ if(i%2) continue; i }"), exact("0\n2\n4"), 0),
    (bc("if(1>2) 5 else 6"), exact("6"), 0),
    (bc("if(2>1) 5 else 6"), exact("5"), 0),
    (bc("if(0) 1"), exact(""), 0),
    (bc("s=0; for(i=1;i<=100;i++) s+=i; s"), exact("5050"), 0),
    (bc("a[0]=1; a[1]=2; a[0]+a[1]"), exact("3"), 0),
    (bc("for(i=0;i<5;i++) a[i]=i*i; a[4]+a[3]"), exact("25"), 0),
    (bc("a[5]"), exact("0"), 0),
    (bc("x=1; y=x+1; x=7; y"), exact("2"), 0),
    (bc("print \"a\", 1+2, \"b\\n\""), exact("a3b"), 0),
    (bc("\"hi\"; 5"), exact("hi5"), 0),
    (bc("1;2;3"), exact("1\n2\n3"), 0),
    (bc("/* c */ 1 # more\n"), exact("1"), 0),
    (bc("2 /* mid */ + 3"), exact("5"), 0),
    # --- functions
    (bc("define f(x){ return x*x }; f(7)"), exact("49"), 0),
    (bc("define f(n){ if(n<2) return 1; return n*f(n-1) }; f(10)"), exact("3628800"), 0),
    (bc("define f(n){ if(n<2) return n; return f(n-1)+f(n-2) }; f(15)"), exact("610"), 0),
    (bc("define f(n){ if(n<2) return 1; return n*f(n-1) }; f(100)", env="BC_LINE_LENGTH=0 "), exact(F100), 0),
    (bc("define f(n){ if(n<2) return 1; return n*f(n-1) }; f(1000)", env="BC_LINE_LENGTH=0 "), exact(F1000), 0),
    (bc("define g(a,b){ return a-b }; g(10,3)"), exact("7"), 0),
    (bc("define g(){ return 5 }; g()+1"), exact("6"), 0),
    (bc("define f(x){ auto y; y=x*2; return y }; y=9; f(4); y"), exact("8\n9"), 0),
    (bc("define f(x){ x=x+1; return x }; a=1; f(a); a"), exact("2\n1"), 0),
    (bc("define s(a[],n){ auto i,t; for(i=0;i<n;i++) t+=a[i]; return t }; a[0]=1;a[1]=2;a[2]=3; s(a[],3)"), exact("6"), 0),
    (bc("define f(x){ return }; f(1)"), exact("0"), 0),
    (bc("define f(x){ if(x>0) return x; return -x }; f(-4)+f(4)"), exact("8"), 0),
    (bc("define void p(x){ print x, \"\\n\" }; p(3)"), exact("3"), 0),
    (bc("define f(n){ auto i; for(i=0;i<n;i++){ if(i==2) return i*10 } return 99 }; f(5)"), exact("20"), 0),
    (bc("define f(x){ return(x+1) }; f(1)"), exact("2"), 0),
    (prog("define f(x) {", "  auto y", "  y = x * 3", "  return (y)", "}", "f(5)"), exact("15"), 0),
    (prog("define p(n) {", "  if (n == 0) return 0", "  return n + p(n - 1)", "}", "p(200)"), exact("20100"), 0),
    (prog("define max(a, b) { if (a > b) return a; return b }", "max(3, 9)", "max(9, 3)"), exact("9\n9"), 0),
    # --- bases
    (bc("obase=16; 255"), exact("FF"), 0),
    (bc("obase=2; 10"), exact("1010"), 0),
    (bc("obase=8; 64"), exact("100"), 0),
    (bc("ibase=16; FF"), exact("255"), 0),
    (bc("ibase=2; 1010"), exact("10"), 0),
    (bc("ibase=8; 777"), exact("511"), 0),
    (bc("obase=16; ibase=16; FF"), exact("FF"), 0),
    (bc("obase=16; 2^64"), exact("10000000000000000"), 0),
    (bc("obase=2; 255"), exact("11111111"), 0),
    (bc("obase=16; 0.5"), exact(".8"), 0),
    (bc("obase=100; 12345"), exact(" 01 23 45"), 0),
    (bc("ibase=A; 10"), exact("10"), 0),
    (bc("obase=16; -255"), exact("-FF"), 0),
    (bc("ibase=16; A0"), exact("160"), 0),
    # --- long numbers wrap like GNU bc
    (bc("2^1000"), exact(wrap(str(2 ** 1000))), 0),
    (bc("2^1000", env="BC_LINE_LENGTH=0 "), exact(str(2 ** 1000)), 0),
    (bc("2^100", env="BC_LINE_LENGTH=20 "), exact(wrap(str(2 ** 100), 20)), 0),
    (bc("10^68"), exact("1" + "0" * 68), 0),
    (bc("10^69"), exact("1" + "0" * 68 + "\\\n0"), 0),
    # --- errors
    ("echo '1/0' | bc 2>&1", "Divide by zero", 0),
    ("echo '1/0; 2+2' | bc 2>&1", "4", 0),
    ("echo '1%0' | bc 2>&1", "by zero", 0),
    ("echo '1+' | bc 2>&1", "syntax error", 0),
    ("echo '1 +* 2' | bc 2>&1", "syntax error", 0),
    ("echo '1+; 5' | bc 2>&1", "syntax error", 0),
    ("echo 'sqrt(-1)' | bc 2>&1", "Square root of a negative number", 0),
    ("echo 'foo(1)' | bc 2>&1", "not defined", 0),
    ("echo 'define f(x){return x}; f(1,2)' | bc 2>&1", "mismatch", 0),
    ("echo 'ibase=1' | bc 2>&1", "ibase", 0),
    ("echo 'a[-1]' | bc 2>&1", "negative", 0),
    ("bc /tmp/does-not-exist; echo status=$?", "status=1", None),
    ("bc --bogus 2>&1; echo status=$?", "status=1", None),
    # --- quit, halt, options, input sources
    (bc("2+3; quit; 4"), exact("5"), 0),
    (bc("quit"), exact(""), 0),
    (bc("if (1) quit; 7"), exact(""), 0),
    (bc("1; halt; 2"), exact("1"), 0),
    ("bc -e '3*3'", exact("9"), 0),
    ("bc -e '1+1' -e '2+2'", exact("2\n4"), 0),
    ("bc --expression='5-1'", exact("4"), 0),
    ("bc -q -e '8'", exact("8"), 0),
    ("echo '9' | bc -q", exact("9"), 0),
    ("echo 5 > /tmp/a.bc; echo 6 > /tmp/b.bc; bc /tmp/a.bc /tmp/b.bc", exact("5\n6"), 0),
    ("echo 'x=4' > /tmp/a.bc; echo 'x*2' | bc /tmp/a.bc -", exact("8"), 0),
    (prog("a=1", "b=2", "a+b"), exact("3"), 0),
    (prog("/* a comment", "spanning lines */", "5"), exact("5"), 0),
    (prog("x = 1 + \\\\", "2", "x"), exact("3"), 0),
    ("bc --version", "bc", 0),
    ("echo '7' | bc /dev/null", exact(""), 0),
    ("printf 'a=1\\nb=2\\n' | bc", exact(""), 0),
    ("echo '1+1' | bc | wc -l", "1", 0),
    # --- bigger computations
    (bc("scale=50; 1/7"), exact("." + ("142857" * 9)[:50]), 0),
    (bc("scale=100; 1/3", env="BC_LINE_LENGTH=0 ") + " | wc -c", "102", 0),
    (bc("3^200 / 3^198"), exact("9"), 0),
    (bc("(10^50 + 1) / (10^25 + 1)"), exact(str((10 ** 50 + 1) // (10 ** 25 + 1))), 0),
    (bc("(10^50 + 7) % (10^25 + 3)"), exact(str((10 ** 50 + 7) % (10 ** 25 + 3))), 0),
    (bc("(2^200 + 12345) / (2^100 + 1)"), exact(str((2 ** 200 + 12345) // (2 ** 100 + 1))), 0),
    (bc("scale=40; (2^130)/(3^50)"), exact(wrap(frac_digits(2 ** 130, 3 ** 50, 40))), 0),
]


def norm(out):
    return out.replace("\r", "")


def check(vm, cmd, expect, status):
    out, rc = test_shell.run(vm, cmd)
    out = norm(out)
    if isinstance(expect, tuple):
        want = expect[1]
        if expect[0] == "==":
            want = want + "\n" if want else ""
        assert out == want, f"expected exactly {want!r}, got {out!r}"
    else:
        assert expect in out, f"expected {expect!r} in {out!r}"
    assert status is None or rc == status, f"exit {rc}, expected {status}; output {out!r}"


def run_table(name, table):
    failures = []
    with Qemu(cmdline="text", name=name) as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        for cmd, expect, status in table:
            try:
                check(vm, cmd, expect, status)
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}: {str(e)[-400:]}")
    assert not failures, f"{len(failures)} of {len(table)} checks failed:\n" + "\n".join(failures)


@test
def bc_language():
    """bc: arithmetic, scale, bases, statements, functions, wrapping and error handling."""
    run_table("t-bc", BC)


def bcl(prog):
    return bc(prog, flags="-l")


PI50 = "3.14159265358979323846264338327950288419716939937510"
PI100 = "3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679"

def four_atan1(scale):
    """4*a(1) the way bc -l computes it: the arc tangent of 1 cut at SCALE digits, times 4 (GNU bc prints
    3.14159265358979323844 at scale 20, the last digits are not those of pi)."""
    n = int(PI100.replace(".", "")[:scale + 1])
    s = str(4 * (n // 4))
    return s[0] + "." + s[1:]


MATH = [
    (bcl("4*a(1)"), exact(four_atan1(20)), 0),
    (bcl("e(1)"), exact("2.71828182845904523536"), 0),
    (bcl("s(1)"), exact(".84147098480789650665"), 0),
    (bcl("c(1)"), exact(".54030230586813971740"), 0),
    (bcl("l(2)"), exact(".69314718055994530941"), 0),
    (bcl("j(0,1)"), exact(".76519768655796655144"), 0),
    (bcl("a(1)"), exact(".78539816339744830961"), 0),
    (bcl("e(2)"), exact("7.38905609893065022723"), 0),
    (bcl("e(-1)"), exact(".36787944117144232159"), 0),
    (bcl("l(10)"), exact("2.30258509299404568401"), 0),
    (bcl("e(0)"), exact("1.00000000000000000000"), 0),
    (bcl("s(0)"), exact("0"), 0),
    (bcl("l(1)"), exact("0"), 0),
    (bcl("a(0)"), exact("0"), 0),
    (bcl("scale=10; s(1)"), exact(".8414709848"), 0),
    (bcl("scale=50; 4*a(1)"), exact(four_atan1(50)), 0),
    (bc("scale=100; 4*a(1)", flags="-l", env="BC_LINE_LENGTH=0 "), exact(four_atan1(100)), 0),
    # pi itself is right to 100 digits when two more digits are computed
    (bc("scale=102; 4*a(1)", flags="-l", env="BC_LINE_LENGTH=0 ") + " | head -c 102", raw(PI100), 0),
    (bcl("scale=30; e(1)"), exact("2.718281828459045235360287471352"), 0),
    (bcl("scale"), exact("20"), 0),
    ("bc --mathlib -e 'e(0)+1'", exact("2.00000000000000000000"), 0),
]


def dc(prog, flags=""):
    return f"printf '%s\\n' '{prog}' | dc {flags}".rstrip()


DC = [
    (dc("2 3 ^ p"), exact("8"), 0),
    (dc("10 k 2 v p"), exact("1.4142135623"), 0),
    (dc("1 2 + p"), exact("3"), 0),
    (dc("5 3 - p"), exact("2"), 0),
    (dc("6 7 * p"), exact("42"), 0),
    (dc("7 2 / p"), exact("3"), 0),
    (dc("5 k 7 2 / p"), exact("3.50000"), 0),
    (dc("2 k 1 3 / p"), exact(".33"), 0),
    (dc("17 5 % p"), exact("2"), 0),
    (dc("17 5 ~ f"), exact("2\n3"), 0),
    (dc("_5 p"), exact("-5"), 0),
    (dc("16 o 255 p"), exact("FF"), 0),
    (dc("16 i FF p"), exact("255"), 0),
    (dc("2 o 10 p"), exact("1010"), 0),
    (dc("3 4 5 f"), exact("5\n4\n3"), 0),
    (dc("1 2 3 z p"), exact("3"), 0),
    (dc("3 d * p"), exact("9"), 0),
    (dc("1 2 r f"), exact("1\n2"), 0),
    (dc("[hello]p"), exact("hello"), 0),
    (dc("[hi]n"), raw("hi"), 0),
    (dc("5 sa 6 la + p"), exact("11"), 0),
    (dc("[2 3 +]x p"), exact("5"), 0),
    (dc("[[yes]p]sa 3 5 >a"), exact("yes"), 0),
    (dc("[[yes]p]sa 5 3 >a"), exact(""), 0),
    (dc("[[eq]p]sa 4 4 =a"), exact("eq"), 0),
    (dc("[[ne]p]sa 4 5 !=a"), exact("ne"), 0),
    (dc("[dn1-d0<a]sa 5 lax"), raw("54321"), 0),
    (dc("5 [d1-d1<f*]sf lfx p"), exact("120"), 0),
    (dc("20 [d1-d1<f*]sf lfx p"), exact(str(math.factorial(20))), 0),
    (dc("[1+d100000>a]sa 0 lax p"), exact("100000"), 0),
    (dc("2 100 ^ p"), exact("1267650600228229401496703205376"), 0),
    (dc("7 3:a 3;a p"), exact("7"), 0),
    (dc("1 2 3 c z p"), exact("0"), 0),
    (dc("12345 Z p"), exact("5"), 0),
    (dc("1.25 X p"), exact("2"), 0),
    (dc("4 13 497 | p"), exact("445"), 0),
    (dc("65 a p"), exact("A"), 0),
    (dc("K p"), exact("0"), 0),
    (dc("5 k K p"), exact("5"), 0),
    (dc("I p"), exact("10"), 0),
    (dc("O p"), exact("10"), 0),
    (dc("1 2 3 4 5 R R p"), exact("3"), 0),
    ("printf '2 3 +\\np\\n' | dc", exact("5"), 0),
    (dc("[a]P"), raw("a"), 0),
    (dc("2 1000 ^ p"), exact(wrap(str(2 ** 1000))), 0),
    (dc("10 k 1 3 / p"), exact(".3333333333"), 0),
    (dc(".5 .25 + p"), exact(".75"), 0),
    (dc("1.5 2 * p"), exact("3.0"), 0),
    (dc("1 Sa 2 Sa La p La p"), exact("2\n1"), 0),
    (dc("1 p q 2 p"), exact("1"), 0),
    (dc("[1 p 1 Q 2 p]x 3 p"), exact("1\n3"), 0),
    (dc("[[1 p q 2 p]x 3 p]x 4 p"), exact("1\n4"), 0),
    ("dc -e '2 3 + p'", exact("5"), 0),
    ("dc -e '2 sa' -e 'la 4 * p'", exact("8"), 0),
    ("echo '6 7 * p' > /tmp/f.dc; dc -f /tmp/f.dc", exact("42"), 0),
    ("echo '6 7 * p' > /tmp/f.dc; dc /tmp/f.dc", exact("42"), 0),
    ("echo '5 p' | dc -e '1 p' -", exact("1\n5"), 0),
    ("dc -V", "dc", 0),
    (dc("+") + " 2>&1", "stack empty", 0),
    (dc("1 0 / p") + " 2>&1", "division by zero", 0),
    (dc("1 0 % p") + " 2>&1", "by zero", 0),
    (dc("0 Q") + " 2>&1", "Q command requires a number >= 1", 0),
    (dc("[a] 1 +") + " 2>&1", "non-numeric", 0),
    (dc("1 2 + p y 5 p") + " 2>&1", "5", 0),
    (dc("_1 v") + " 2>&1", "negative", 0),
    ("dc /tmp/no-such-file; echo status=$?", "status=1", None),
    ("dc --bogus 2>&1; echo status=$?", "status=1", None),
    ("echo '5' | dc -e '? p'", exact("5"), 0),
    (dc("3 4 + p # comment"), exact("7"), 0),
    (dc("[x]sa [y]sb la p lb p"), exact("x\ny"), 0),
    (dc("2 1 <a z p") + " 2>&1", "empty", 0),
]


@test
def bc_mathlib():
    """bc -l: sine, cosine, arc tangent, logarithm, exponential and Bessel values, pi to 100 digits."""
    run_table("t-bc-math", MATH)


@test
def dc_stack_machine():
    """dc: arithmetic, scale and bases, registers, arrays, macros and loops, quit levels, errors, options."""
    run_table("t-dc", DC)


@test
def bc_dc_manual_pages():
    """bc and dc: the manual pages exist, parse and are listed."""
    with Qemu(cmdline="text", name="t-bc-man") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = test_shell.run(vm, "patchouli --check-all", timeout=90)
        assert rc == 0 and "0 problems" in out, out
        out, rc = test_shell.run(vm, "patchouli -l | grep -E '^(bc|dc)[(]'")
        assert "bc(1)" in out and "dc(1)" in out, out
