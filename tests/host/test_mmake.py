"""mmake against GNU make (host test, Linux or WSL: needs gcc and GNU make).

  python tools/hosttest.py discover -s tests/host -p "test_mmake.py" -v

Every case writes the same files into two fresh directories, runs `make` in one and `mmake` in the other with the same arguments, and
requires the same output (after the program name and the "[Makefile:N: target]" part of an error line are made equal), the same exit
status and the same files afterwards.  The mtimes of the files a case creates are set explicitly, so up-to-date checks do not depend on
how fast this PC is.  The cases are written to stay inside what mmake supports (docs/SELFHOST.md lists what it leaves out).
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "user" / "bin" / "mmake" / "mmake.c"
OUT = ROOT / "build" / "host-mmake"
MMAKE = OUT / "mmake"
GNU = shutil.which("make")
CC = shutil.which("gcc") or shutil.which("cc")


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    if not MMAKE.exists() or MMAKE.stat().st_mtime < SRC.stat().st_mtime:
        r = subprocess.run([CC, "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra",
                            "-Wno-unused-parameter", "-o", str(MMAKE), str(SRC)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("building mmake failed:\n" + r.stdout + r.stderr)


def norm(text, prog, d):
    text = text.replace(str(d), "DIR").replace(d.name, "DIR")
    text = re.sub(r"^%s(\[\d+\])?: " % re.escape(prog), "make: ", text, flags=re.M)
    text = re.sub(r"\[(?:[^\]:]+):\d+: ([^\]]+)\]", r"[\1]", text)       # GNU: [Makefile:3: all]   mmake: [all]
    return text.replace("\r", "")


class Case:
    """files: name -> text or (text, mtime); runs: argument lists (each is one invocation in the same directory, in order)."""

    def __init__(self, name, makefile, runs=((),), files=None, post=()):
        self.name, self.makefile, self.runs, self.files, self.post = name, makefile, runs, files or {}, post


def populate(d, case):
    (d / "Makefile").write_text(case.makefile)
    for name, spec in case.files.items():
        text, mtime = spec if isinstance(spec, tuple) else (spec, None)
        p = d / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        os.utime(p, (mtime or 1_700_000_000, mtime or 1_700_000_000))


def run_case(case, prog):
    d = Path(tempfile.mkdtemp(prefix="mmake-test-"))
    try:
        populate(d, case)
        results = []
        for args in case.runs:
            r = subprocess.run([prog, *args], cwd=d, capture_output=True, text=True, timeout=60, env={**os.environ, "MAKEFLAGS": "", "LC_ALL": "C", "ASAN_OPTIONS": "detect_leaks=0"})
            name = "mmake" if prog == str(MMAKE) else "make"
            results.append((r.returncode, norm(r.stdout, name, d), norm(r.stderr, name, d)))
        files = sorted(str(p.relative_to(d)) for p in d.rglob("*") if p.is_file())
        contents = {f: (d / f).read_bytes() for f in files if f != "Makefile"}
        return results, files, contents
    finally:
        shutil.rmtree(d, ignore_errors=True)


OLD, NEW = 1_700_000_000, 1_700_000_500

CASES = [
    Case("variables_flavors", """
A = $(B)
B = one
C := $(B)
B = two
D ?= first
D ?= second
E = a
E += b c
F := x
F += $(B)
G != echo shell-made
all: ; @echo A=$(A) C=$(C) D=$(D) E=$(E) F=$(F) G=$(G)
"""),
    Case("variables_command_line_wins", "X = file\nall: ; @echo X=$(X) Y=$(Y)\n", runs=[("X=cmd", "Y=also"), ("-s",)]),
    Case("variables_override", "override X = file\nY = file\nall: ; @echo X=$(X) Y=$(Y)\n", runs=[("X=cmd", "Y=cmd")]),
    Case("variables_environment", "all: ; @echo HOME_SET=$(if $(FOO_FROM_ENV),yes,no) $(FOO_FROM_ENV) $(origin FOO_FROM_ENV)\n", runs=[("FOO_FROM_ENV=1",)]),
    Case("substitution_refs", """
SRCS = a.c b.c dir/c.c
all: ; @echo $(SRCS:.c=.o) / $(SRCS:%.c=obj/%.o) / $(SRCS:.x=.y) / $(SRCS:dir/%=%)
"""),
    Case("functions_text", """
L = The quick  brown fox jumps
all:
	@echo '$(subst quick,slow,$(L))' '$(patsubst %o%,[%],$(L))' '$(strip   a   b  )'
	@echo '$(findstring brown,$(L))|$(findstring cat,$(L))' '$(filter %k %x,$(L))' '$(filter-out The,$(L))'
	@echo '$(sort z a b a)' '$(word 2,$(L))' '$(wordlist 2,3,$(L))' '$(words $(L))' '$(firstword $(L))' '$(lastword $(L))'
	@echo '$(dir src/a.c b.c /x)' '$(notdir src/a.c b.c /x/)' '$(suffix a.c b d.tar.gz)' '$(basename src/a.c b d.tar.gz x.d/y)'
	@echo '$(addsuffix .o,a b c)' '$(addprefix src/,a b)' '$(join a b c,1 2)'
"""),
    Case("functions_logic", """
E =
V = yes
all:
	@echo '$(if $(V),T,F)|$(if $(E),T,F)|$(if $(E),T)|$(or $(E),$(V),other)|$(and $(V),last)|$(and $(E),last)|'
	@echo '$(foreach x,a b c,[$(x)])' '$(foreach x,1 2,$(foreach y,a b,$(x)$(y)))'
	@echo '$(origin V)|$(origin NOPE)|$(origin CC)|$(origin MAKE)|$(flavor V)|$(flavor E)|$(value V)'
"""),
    Case("functions_call", """
swap = $(2) $(1)
pair = <$(1),$(2),$(0)>
all: ; @echo '$(call swap,a,b)' '$(call pair,x)' '$(call pair,x,y,z)'
"""),
    Case("functions_shell_wildcard", """
all:
	@echo '$(shell echo hi; echo there)'
	@echo '$(wildcard *.c)' '$(wildcard src/*.h)' '$(wildcard *.nothing)' '$(wildcard a.c nonexistent.c)' '$(wildcard s*/*.h)'
""", files={"b.c": "", "a.c": "", "src/x.h": "", "src/y.h": "", "src/z.txt": ""}),
    Case("conditionals", """
A = 1
B =
ifeq ($(A),1)
R1 = a-is-one
else
R1 = a-is-not-one
endif
ifneq "$(A)" "1"
R2 = bad
else ifeq '$(B)' ''
R2 = b-empty
else
R2 = b-set
endif
ifdef A
R3 = A-defined
endif
ifdef B
R3 += B-defined
endif
ifndef NOTHING
R4 = nothing-undefined
endif
ifeq (a b, a b )
R5 = trimmed
endif
all: ; @echo $(R1) $(R2) $(R3) $(R4) $(R5)
"""),
    Case("conditionals_nested_skipped", """
ifeq (1,2)
  ifeq (1,1)
X = inner-should-not-run
  endif
Y = nor-this
else
X = right
endif
all: ; @echo X=$(X) Y=$(Y)
"""),
    Case("define_endef", """
define greeting
hello
there
endef
define RULETEXT
@echo recipe-from-define
endef
V := $(greeting)
all:
	$(RULETEXT)
	@echo '$(V)'
"""),
    Case("include", """
include extra.mk
-include missing.mk
all: ; @echo $(FROM_EXTRA) $(TARGET_LIST)
""", files={"extra.mk": "FROM_EXTRA = extra-was-here\nTARGET_LIST = t1 t2\n"}),
    Case("include_missing_is_an_error", "include nothere.mk\nall: ; @echo no\n"),
    Case("echo_and_prefixes", """
all:
	echo shown
	@echo hidden-command
	-false
	@echo after-ignored-failure
"""),
    Case("failure_stops", """
all:
	@echo before
	false
	@echo never
""", runs=[(), ("-i",), ("-k",)]),
    Case("failure_exit_codes", "all: ; @exit 3\n"),
    Case("keep_going", """
all: good bad also
good: ; @echo good
bad: ; @echo bad; false
also: ; @echo also
""", runs=[("-k",), ()]),
    Case("up_to_date_by_mtime", """
out.txt: in.txt
	@echo building out.txt from $<
	@cp in.txt out.txt
""", files={"in.txt": ("data\n", OLD), "out.txt": ("old\n", NEW)}, runs=[(), ("-B",)]),
    Case("rebuild_by_mtime", """
out.txt: in.txt
	@echo building out.txt from $<
	@cp in.txt out.txt
""", files={"in.txt": ("data\n", NEW), "out.txt": ("old\n", OLD)}, runs=[(), ()]),
    Case("chain_of_files", """
all: c.txt
c.txt: b.txt
	@echo c; cp b.txt c.txt
b.txt: a.txt
	@echo b; cp a.txt b.txt
""", files={"a.txt": ("a\n", NEW), "b.txt": ("b\n", OLD), "c.txt": ("c\n", OLD)}),
    Case("missing_prerequisite_without_rule", "all: nothere\n\t@echo built\n"),
    Case("missing_goal", "all: ; @echo hi\n", runs=[("nosuch",)]),
    Case("nothing_to_be_done", "all: a\n.PHONY: all\n", files={"a": ("", OLD)}, runs=[(), ("a",)]),
    Case("phony_always_runs", """
.PHONY: clean hello
hello: ; @echo hello
clean: ; @echo cleaning
all: hello clean
""", files={"clean": ("a file named clean\n", NEW), "hello": ("", NEW)}, runs=[("all",), ("clean",)]),
    Case("default_goal_first_target", ".hidden: ; @echo nope\nfirst: second ; @echo first\nsecond: ; @echo second\n"),
    Case("default_goal_variable", ".DEFAULT_GOAL := second\nfirst: ; @echo first\nsecond: ; @echo second\n"),
    Case("pattern_rule", """
%.o: %.c
	@echo cc $< -o $@
	@cp $< $@
prog: a.o b.o
	@echo link $^ to $@
""", files={"a.c": ("a\n", NEW), "b.c": ("b\n", NEW)}, runs=[("prog",), ("prog",)]),
    Case("pattern_rule_stem_and_dirs", """
out/%.txt: in/%.src
	@echo stem=$* target=$@ dir=$(@D) file=$(@F) first=$< dirs=$(<D) $(<F)
	@mkdir -p $(@D)
	@cp $< $@
all: out/one.txt out/two.txt
""", files={"in/one.src": ("1\n", NEW), "in/two.src": ("2\n", NEW)}),
    Case("pattern_rule_shortest_stem", """
%.x: ; @echo generic $@ stem=$*
a%.x: ; @echo a-rule $@ stem=$*
all: abc.x bcd.x
"""),
    Case("pattern_rule_needs_prereqs_to_exist", """
%.o: %.c ; @echo compile $@
%.o: %.s ; @echo assemble $@
all: one.o two.o
""", files={"one.c": "", "two.s": ""}),
    Case("pattern_rule_chain", """
%.o: %.c ; @echo cc $@ ; touch $@
%.c: %.y ; @echo yacc $@ ; touch $@
all: p.o p.c
""", files={"p.y": ("", NEW)}),
    Case("recipe_with_several_lines_in_a_variable", """
define two
@echo first line
@echo second line
endef
all: ; $(two)
"""),
    Case("pattern_rule_override_builtin", """
%.o: %.c
	@echo my own rule for $@
all: x.o
""", files={"x.c": ("", NEW)}),
    Case("builtin_rule_uses_cc", """
CC = echo
CFLAGS = -O2
all: x.o
""", files={"x.c": ("", NEW)}),
    Case("static_pattern_rule", """
OBJS = a.o b.o
$(OBJS): %.o: %.c common.h
	@echo build $@ from $< and $^ stem=$*
all: $(OBJS)
""", files={"a.c": "", "b.c": "", "common.h": ""}),
    Case("automatic_variables", """
t: dup1 dup2 dup1 | oo
	@echo '@=$@ <=$< ^=$^ +=$+ ?=$?'
	@echo 'D=$(@D) F=$(@F) <D=$(<D) <F=$(<F) ^D=$(^D)'
dup1 dup2 oo: ; @:
""", files={"t": ("", NEW), "dup1": ("", OLD), "dup2": ("", NEW), "oo": ("", NEW)}),
    Case("automatic_newer_prerequisites", """
t: a b c
	@echo 'newer: $?'
""", files={"t": ("", NEW), "a": ("", OLD), "b": ("", NEW + 10), "c": ("", NEW + 20)}),
    Case("order_only", """
out: in | dir
	@echo make out from $^ ordered $(filter-out $^,$|)
dir: ; @echo make dir
""", files={"in": ("", OLD), "out": ("", NEW)}),
    Case("several_targets", """
a b c: ; @echo making $@
all: a b c
"""),
    Case("prerequisites_from_two_rules", """
t: a
t: b
	@echo t from $^
a b: ; @echo $@
"""),
    Case("recipe_replaced_by_later_rule", "t: ; @echo first\nt: ; @echo second\n"),
    Case("recipe_on_the_rule_line", "t: a ; @echo inline $<\na: ; @echo a\n"),
    Case("recipe_continuation", "t:\n\t@echo one \\\n\t   two \\\n\t   three\n\t@echo a; \\\n\techo b\n"),
    Case("variable_continuation", "L = a \\\n    b \\\n    c\nall: ; @echo '$(L)' '$(words $(L))'\n"),
    Case("dollars_in_recipes", "V = make-var\nall:\n\t@x=shell-var; echo $$x $(V) $$$$ | sed 's/[0-9]*$$/PID/'\n\t@echo '$$(not a var)'\n"),
    Case("comments", "# a comment\nV = a # trailing comment\nW = b \\# not a comment\nall: ; @echo '$(V)|$(W)' # shell sees the comment too\n\n# another\n"),
    Case("empty_and_blank_lines_in_recipes", "all:\n\t@echo one\n\n# comment between\n\n\t@echo two\n"),
    Case("exports", "export FOO = from-make\nBAR = not-exported\nexport BAZ := baz\nall: ; @echo \"$$FOO $$BAR $$BAZ\"\n"),
    Case("export_command_line", "all: ; @echo \"[$$CMDV]\"\n", runs=[("CMDV=hello",)]),
    Case("dry_run", "t: a\n\techo build $@\n\t@echo quiet $<\n\t-false\na: ; touch a\n", runs=[("-n",), ("-n", "t")], files={}),
    Case("silent_flag", "all:\n\techo loud\n", runs=[("-s",), ()]),
    Case("directory_flag", "all: ; @echo in `basename $$PWD`\n", runs=[("-C", ".")]),
    Case("file_flag", "all: ; @echo default\n", files={"other.mk": "all: ; @echo other\n"}, runs=[("-f", "other.mk"), ("-f", "Makefile")]),
    Case("circular_dependency", "a: b ; @echo a\nb: a ; @echo b\n"),
    Case("makecmdgoals", "all x: ; @echo goals=$(MAKECMDGOALS)\n", runs=[("x",), ("all", "x")]),
    Case("missing_separator", "this is not a rule\nall: ; @echo x\n"),
    Case("unterminated_reference", "A = $(unclosed\nall: ; @echo x\n"),
    Case("recursive_variable_error", "A = $(B)\nB = $(A)\nall: ; @echo $(A)\n"),
    Case("error_function", "all: ; @echo before\n$(error stop right here)\n"),
    Case("info_function", "$(info parse time message)\nall: ; @echo run time\n"),
    Case("targets_with_dirs_and_spaces_in_values", "DIR = build/obj\n$(DIR)/x.o: ; @mkdir -p $(@D) && echo made $@\nall: $(DIR)/x.o\n"),
    Case("make_variable_forms", "x = 1\nall: ; @echo $x ${x} $(x) $(x:1=2)\n"),
    Case("target_names_in_variables", "T = real\n$(T): ; @echo making $@\nall: $(T)\n"),
    Case("wildcard_in_prerequisites", "SRC = $(wildcard src/*.c)\nOBJ = $(SRC:src/%.c=%.o)\nall: $(OBJ)\n%.o: src/%.c ; @echo cc $< $@\n",
         files={"src/a.c": "", "src/b.c": ""}),
]


@unittest.skipUnless(GNU and CC, "needs GNU make and a C compiler (gcc) on the PATH")
class MmakeVersusGnu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    def test_cases(self):
        failures = []
        for case in CASES:
            want = run_case(case, GNU)
            got = run_case(case, str(MMAKE))
            if want != got:
                failures.append(self.describe(case, want, got))
        self.assertFalse(failures, "\n\n".join(failures))

    @staticmethod
    def describe(case, want, got):
        lines = [f"--- {case.name}"]
        for i, (w, g) in enumerate(zip(want[0], got[0])):
            if w != g:
                lines.append(f"run {i} {case.runs[i]}:\n  make : rc={w[0]}\n{w[1]}{w[2]}\n  mmake: rc={g[0]}\n{g[1]}{g[2]}")
        if want[1] != got[1]:
            lines.append(f"files: make {want[1]} / mmake {got[1]}")
        for f in want[2]:
            if f in got[2] and want[2][f] != got[2][f]:
                lines.append(f"file {f}: make {want[2][f]!r} / mmake {got[2][f]!r}")
        return "\n".join(lines)


if __name__ == "__main__":
    unittest.main()
