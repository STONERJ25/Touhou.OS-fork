#!/usr/bin/env python3
"""Host tests for the web engine (user/lib/web), through tests/host/webtool.c (built with AddressSanitizer and UBSan).

    python3 -m unittest discover -s tests/host -p "test_web.py" -v          (on Windows: python tools/hosttest.py ...)

The cases are the repository's own (tests/web/: tokenizer tests in the html5lib JSON format, tree-construction tests in
the html5lib .dat format, CSS, layout and paint tables, sample pages). When the html5lib tokenizer tests and the
web-platform-tests tree-construction tests have been fetched into build/web-fetch/ (never committed: docs/WEB.md says how),
the conformance classes run them too and print the pass rates.
"""
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
WEB = REPO / "user" / "lib" / "web"
FIX = REPO / "tests" / "web"
FETCH = REPO / "build" / "web-fetch"


def _in_wsl():
    try:
        return "microsoft" in os.uname().release.lower()
    except AttributeError:
        return False


OUT = REPO / "build" / ("host-web-wsl" if _in_wsl() else "host-web")


def find_cc():
    import shutil
    return shutil.which("gcc") or shutil.which("clang")


def build(name="webtool", sanitize=True, extra=()):
    """Builds webtool from the library's sources (rebuilt when any source is newer). Returns its path."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / (name + ("" if sanitize else "-fast"))
    # the engine, the tool, and the libraries the image adapter (imgadapt.c) and the reference rasteriser use
    libs = sorted((REPO / "user" / "lib" / "img" / "src").glob("*.c")) + sorted((REPO / "user" / "lib" / "z" / "src").glob("*.c"))
    srcs = sorted((WEB / "src").glob("*.c")) + [HERE / "webtool.c"] + [REPO / "shared" / "font8x16.c"] + list(extra)
    deps = srcs + libs + sorted((WEB / "src").glob("*.h")) + sorted((WEB / "include").glob("*.h"))
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = find_cc()
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize else ["-O2", "-g", "-DNDEBUG"]
    inc = ["-I", str(WEB / "include"), "-I", str(REPO / "shared"), "-I", str(REPO / "user" / "lib" / "img" / "include"),
           "-I", str(REPO / "user" / "lib" / "z" / "include")]
    objs = []
    for lib in libs:                                         # other agents' code: compiled without -Werror
        obj = OUT / ("lib-%s-%s%s.o" % (lib.parent.parent.name, lib.stem, "" if sanitize else "-fast"))
        if not obj.exists() or obj.stat().st_mtime < lib.stat().st_mtime:
            r = subprocess.run([cc, *flags, "-std=gnu17", "-w", *inc, "-c", str(lib), "-o", str(obj)], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("build of %s failed:\n%s%s" % (lib, r.stdout, r.stderr))
        objs.append(obj)
    cmd = [cc, *flags, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-Wno-missing-field-initializers", *inc, *map(str, srcs), *map(str, objs), "-lm", "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of %s failed:\n%s%s" % (name, r.stdout, r.stderr))
    return exe


def run(cmds, fast=False, timeout=900):
    """cmds: [(args, payload bytes)]; returns the answers (bytes), one per command."""
    exe = build(sanitize=not fast)
    data = b"".join(("%s %d\n" % (" ".join(args), len(p))).encode() + p for args, p in cmds)
    r = subprocess.run([str(exe)], input=data, capture_output=True, timeout=timeout,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1"))
    if r.returncode != 0:
        raise RuntimeError("webtool failed (%d): %s" % (r.returncode, r.stderr[-4000:].decode(errors="replace")))
    out, res, i = r.stdout, [], 0
    while i < len(out):
        j = out.index(b"\n", i)
        n = int(out[i:j])
        res.append(out[j + 1:j + 1 + n])
        i = j + 1 + n
    return res


def wtf8(s):
    return s.encode("utf-8", "surrogatepass")


# ================================================================================================== the tokenizer

STATES = {"Data state": "data", "PLAINTEXT state": "plaintext", "RCDATA state": "rcdata", "RAWTEXT state": "rawtext",
          "Script data state": "script", "CDATA section state": "cdata"}


def unescape_double(s):
    """html5lib "doubleEscaped": every string has its \\uHHHH sequences decoded once more (they may be lone surrogates)."""
    if isinstance(s, str):
        return re.sub(r"\\u([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), s)
    if isinstance(s, list):
        return [unescape_double(x) for x in s]
    if isinstance(s, dict):
        return {unescape_double(k): unescape_double(v) for k, v in s.items()}
    return s


def normalize_tokens(tokens):
    """Coalesce adjacent character tokens (the html5lib comparison rule)."""
    out = []
    for t in tokens:
        if t[0] == "Character" and out and out[-1][0] == "Character":
            out[-1] = ["Character", out[-1][1] + t[1]]
        else:
            out.append(list(t))
    return out


def tokenizer_cases(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    for i, t in enumerate(data.get("tests", data.get("xmlViolationTests", []))):
        if "xmlViolationTests" in data:
            continue
        inp, out = t["input"], t["output"]
        if t.get("doubleEscaped"):
            inp = unescape_double(inp)
            out = unescape_double(out)
        for st in t.get("initialStates", ["Data state"]):
            yield ("%s #%d %s (%s)" % (path.name, i, t["description"], st), inp, STATES[st], t.get("lastStartTag"), out,
                   [e["code"] for e in t.get("errors", [])])


def run_tokenizer_cases(cases):
    cmds = [(["tokens", st, last or "-"], wtf8(inp)) for _, inp, st, last, _, _ in cases]
    answers = run(cmds)
    fails, err_fails = [], 0
    for (name, inp, st, last, want, want_errs), ans in zip(cases, answers):
        toks, errs = ans.decode("utf-8", "surrogatepass").split("\n", 1)
        got = normalize_tokens(json.loads(toks))
        if got != normalize_tokens(want):
            fails.append((name, inp, want, got))
        if sorted(e["code"] for e in json.loads(errs)) != sorted(want_errs):
            err_fails += 1
    return fails, err_fails


class Tokenizer(unittest.TestCase):
    def test_own_cases(self):
        cases = [c for p in sorted((FIX / "tokenizer").glob("*.test")) for c in tokenizer_cases(p)]
        self.assertTrue(cases)
        fails, err_fails = run_tokenizer_cases(cases)
        self.assertEqual(fails, [], "\n".join("%s\n  input %r\n  want %r\n  got  %r" % f for f in fails[:10]))
        self.assertEqual(err_fails, 0, "parse error codes differ")

    @unittest.skipUnless((FETCH / "html5lib-tests" / "tokenizer").is_dir(), "html5lib-tests not fetched (docs/WEB.md)")
    def test_html5lib_conformance(self):
        cases = [c for p in sorted((FETCH / "html5lib-tests" / "tokenizer").glob("*.test")) for c in tokenizer_cases(p)]
        fails, err_fails = run_tokenizer_cases(cases)
        print("\nhtml5lib tokenizer: %d/%d token streams match, %d/%d error lists match" %
              (len(cases) - len(fails), len(cases), len(cases) - err_fails, len(cases)), file=sys.stderr)
        for f in fails[:15]:
            print("  FAIL %s\n    input %r\n    want %r\n    got  %r" % f, file=sys.stderr)
        self.assertEqual(len(fails), 0)


# ================================================================================================== tree construction

def read_dat(path):
    """The bytes as they are: a CR inside a test is data (Path.read_text would turn it into LF)."""
    return path.read_bytes().decode("utf-8", "surrogateescape")


def parse_dat(text):
    tests, cur, sec = [], None, None
    heads = {"#data", "#errors", "#new-errors", "#document-fragment", "#document", "#script-off", "#script-on", "#description"}
    for line in text.split("\n"):
        if line in heads:
            # a test starts at "#description" (which comes first when present) or at "#data" after a complete test
            if line == "#description" or (line == "#data" and (cur is None or "data" in cur)):
                if cur:
                    tests.append(cur)
                cur = {}
            sec = line[1:]
            cur.setdefault(sec, [])
            continue
        if cur is not None and sec:
            cur[sec].append(line)
    if cur is not None:
        tests.append(cur)
    res = []
    for t in tests:
        doc = t.get("document", [])
        while doc and doc[-1] == "":
            doc.pop()
        frag = "\n".join(t.get("document-fragment", [])).strip() or None
        if frag and " " in frag:
            ns, name = frag.split(" ", 1)
            frag = ns + ":" + name
        res.append(dict(data="\n".join(t.get("data", [])), doc="\n".join(doc), frag=frag, script="script-on" in t))
    return res


def run_tree_cases(cases, fast=False):
    cmds = []
    for c in cases:
        opts = []
        if c["script"]:
            opts.append("script")
        if c["frag"]:
            opts.append("frag=" + c["frag"])
        cmds.append((["tree", ",".join(opts) or "-"], c["data"].encode("utf-8", "surrogateescape")))
    answers = run(cmds, fast=fast)
    fails = []
    for c, a in zip(cases, answers):
        got = a.decode("utf-8", "surrogatepass").rstrip("\n")
        if got != c["doc"]:
            fails.append((c, got))
    return fails


def show_tree_fail(c, got):
    return "data %r (frag %s, script %s)\n--- want\n%s\n--- got\n%s" % (c["data"], c["frag"], c["script"], c["doc"], got)


class TreeConstruction(unittest.TestCase):
    def test_own_cases(self):
        cases = []
        for p in sorted((FIX / "tree").glob("*.dat")):
            for c in parse_dat(read_dat(p)):
                c["file"] = p.name
                cases.append(c)
        self.assertTrue(cases)
        fails = run_tree_cases(cases)
        self.assertEqual([], [show_tree_fail(*f) for f in fails[:8]])

    @unittest.skipUnless((FETCH / "wpt" / "html" / "syntax" / "parsing" / "resources").is_dir(), "WPT parsing tests not fetched")
    def test_wpt_conformance(self):
        res = FETCH / "wpt" / "html" / "syntax" / "parsing" / "resources"
        total, failed, per = 0, 0, []
        allfails = []
        for p in sorted(res.glob("*.dat")):
            if p.name.startswith("scripted_"):
                continue                                     # they run JavaScript (document.getElementById(...).id = ...)
            cases = parse_dat(read_dat(p))
            fails = run_tree_cases(cases)
            total += len(cases)
            failed += len(fails)
            per.append((p.name, len(cases) - len(fails), len(cases)))
            allfails += [(p.name, f) for f in fails]
        print("\ntree construction (WPT html/syntax/parsing): %d/%d pass" % (total - failed, total), file=sys.stderr)
        for name, ok, n in per:
            if ok != n:
                print("  %-40s %d/%d" % (name, ok, n), file=sys.stderr)
        for name, (c, got) in allfails[:int(os.environ.get("WEB_SHOW", "6"))]:
            print("FAIL %s: %s" % (name, show_tree_fail(c, got)), file=sys.stderr)
        self.assertLessEqual(failed, int(os.environ.get("WEB_TREE_MAX_FAIL", "0")))


# ================================================================================================== CSS

def blocks(path):
    """'=== name [opts]' blocks: (name, opts, document text, [(left, right)]) with lines 'left => right' after the
    document (the document is every line before the first '=>' line)."""
    out, cur = [], None
    for line in path.read_text(encoding="utf-8").split("\n"):
        if line.startswith("#") and cur is None:
            continue
        if line.startswith("=== "):
            title = line[4:]
            opts = ""
            m = re.match(r"(.*?)\s*\[(.*)\]\s*$", title)
            if m:
                title, opts = m.group(1), m.group(2)
            cur = [title, opts, [], []]
            out.append(cur)
            continue
        if cur is None or not line.strip():
            continue
        if "=>" in line and (cur[3] or not line.startswith("<")):
            left, right = line.rsplit("=>", 1)
            cur[3].append((left.strip(), right.strip()))
        else:
            cur[2].append(line)
    return [(t, o, "\n".join(d), cases) for t, o, d, cases in out]


class Selectors(unittest.TestCase):
    def test_specificity(self):
        cases = []
        for line in (FIX / "css" / "specificity.txt").read_text(encoding="utf-8").split("\n"):
            if not line.strip() or line.startswith("#"):
                continue
            sel, want = line.rsplit("=>", 1)
            cases.append((sel.strip(), want.strip()))
        answers = run([(["spec"], s.encode()) for s, _ in cases])
        bad = [(s, w, a.decode()) for (s, w), a in zip(cases, answers) if a.decode() != w]
        self.assertEqual(bad, [])

    def test_matching(self):
        cmds, want = [], []
        for title, opts, doc, cases in blocks(FIX / "css" / "match.txt"):
            for sel, exp in cases:
                cmds.append((["select", opts or "-"], (sel + "\n" + doc).encode()))
                want.append((title, sel, "" if exp == '""' else exp))
        answers = run(cmds)
        bad = [(t, s, w, a.decode()) for (t, s, w), a in zip(want, answers) if a.decode() != w]
        self.assertEqual(bad, [])


class Cascade(unittest.TestCase):
    def test_tables(self):
        cmds, want = [], []
        for title, opts, doc, cases in blocks(FIX / "css" / "cascade.txt"):
            for left, exp in cases:
                sel, props = [x.strip() for x in left.split("|", 1)]
                cmds.append((["style", opts.replace(" ", ",") or "-"], (sel + "\n" + props + "\n" + doc).encode()))
                want.append((title, left, exp))
        answers = run(cmds)
        bad = []
        for (title, left, exp), a in zip(want, answers):
            lines = [l for l in a.decode().split("\n") if l]
            got = " || ".join(" ; ".join(kv.split("=", 1)[1] for kv in l.split("\t")[1:]) for l in lines)
            if got != exp:
                bad.append((title, left, exp, got, a.decode()))
        self.assertEqual(bad, [], "\n".join("%s: %s\n  want %s\n  got  %s\n  raw %r" % b for b in bad[:12]))


# ================================================================================================== layout

def layout_cases(path):
    """'=== name [opts]' cases: the page up to a '---' line, then the lines the layout must print."""
    out, cur, in_doc = [], None, False
    for line in path.read_text(encoding="utf-8").split("\n"):
        if line.startswith("#") and cur is None:
            continue
        if line.startswith("=== "):
            title, opts = line[4:], ""
            m = re.match(r"(.*?)\s*\[(.*)\]\s*$", title)
            if m:
                title, opts = m.group(1), m.group(2)
            cur = [title, opts, [], []]
            out.append(cur)
            in_doc = True
            continue
        if cur is None:
            continue
        if in_doc and line.strip() == "---":
            in_doc = False
            continue
        if in_doc:
            cur[2].append(line)
        elif line.strip():
            cur[3].append(line.strip())
    return [(t, o, "\n".join(d), want) for t, o, d, want in out]


class Layout(unittest.TestCase):
    """Box positions and line boxes against values worked out by hand (tests/web/layout/*.txt)."""

    def check_file(self, name):
        cases = layout_cases(FIX / "layout" / name)
        answers = run([(["layout", o.replace(" ", ",") or "-"], d.encode()) for _, o, d, _ in cases])
        bad = []
        for (title, opts, doc, want), got in zip(cases, answers):
            lines = set(got.decode(errors="replace").split("\n"))
            missing = [w for w in want if w not in lines]
            if missing:
                bad.append("%s: missing %s\n    got: %s" % (title, missing, " | ".join(l for l in got.decode(errors="replace").split("\n")
                                                                                       if l and any(l.split(" ")[0] == m.split(" ")[0] for m in missing))))
        self.assertEqual(bad, [], "\n" + "\n".join(bad))
        return len(cases)

    def test_block(self):
        self.check_file("block.txt")

    def test_inline(self):
        self.check_file("inline.txt")

    def test_flex_grid(self):
        self.check_file("flexgrid.txt")

    def test_tables(self):
        self.check_file("table.txt")


class Interaction(unittest.TestCase):
    """Hit testing, find in page, painting order."""

    def test_hit(self):
        page = (b'<style>body{margin:0} #z1{position:absolute;left:0;top:100px;width:50px;height:50px;z-index:2}'
                b' #z2{position:absolute;left:0;top:100px;width:50px;height:50px;z-index:1} #np{pointer-events:none}</style>'
                b'<p id=p style="margin:0">hello <a id=l href="x/y.html">link text</a></p>'
                b'<div id=z1></div><div id=z2></div><input id=i style="position:absolute;top:200px;left:0">'
                b'<label for=i id=lab style="position:absolute;top:300px;left:0">Label</label>')
        got = [a.decode() for a in run([(["hit", "-"], b"%d %d\n" % xy + page) for xy in
                                         ((19, 5), (59, 5), (10, 120), (5, 205), (10, 305), (700, 500))])]
        self.assertEqual(got[0], "node=#text offset=2 element=p#p cursor=2")
        self.assertEqual(got[1], "node=#text offset=1 element=a#l href=http://example.org/dir/x/y.html cursor=1")
        self.assertTrue(got[2].startswith("node=div#z1 "), got[2])
        self.assertIn("control=input#i", got[3])
        self.assertIn("control=input#i", got[4])
        self.assertTrue(got[5].startswith("node=body"), got[5])

    def test_find(self):
        page = b"<p>The quick brown fox.</p><p>Quick <b>br</b>own again</p>"
        got = run([(["find", "-"], b"quick brown\n" + page), (["find", "which=1"], b"quick brown\n" + page)])
        self.assertEqual(got[0].decode(), 'count=2 rect=40,16,88,16 selected="quick brown"')
        self.assertTrue(got[1].decode().startswith("count=2 rect=8,48,"), got[1])
        self.assertIn('selected="Quick brown"', got[1].decode())

    def test_paint_order(self):
        page = (b'<style>body{margin:0} div{width:50px;height:50px} #a{background:red;position:relative;z-index:2}'
                b' #b{background:blue;position:relative;top:-25px;z-index:1} #c{background:green;float:left}</style>'
                b'<div id=a></div><div id=b></div><div id=c></div><span style="background:yellow">t</span>')
        items = [l for l in run([(["paint", "-"], page)])[0].decode().split("\n") if l.startswith(("rect", "text"))]
        order = [l.split()[-1] if l.startswith("rect") else "text" for l in items]
        # in-flow inline content (the yellow span and its text) comes before the positioned boxes, z-index 1 before 2
        self.assertEqual(order, ["div#c", "span", "text", "div#b", "div#a"], items)


def random_page(rnd, n):
    """A random page: nested elements of every display type, styles, text, entities, broken markup."""
    tags = ["div", "span", "p", "a", "b", "table", "tr", "td", "ul", "li", "img", "input", "select", "option", "br", "pre",
            "h1", "section", "button", "textarea", "svg", "math", "template", "iframe", "ol", "dl", "dt", "dd", "center"]
    styles = ["display:flex", "display:grid;grid-template-columns:1fr 2fr 50px", "display:inline-block", "float:left",
              "float:right", "position:absolute;top:3px", "position:relative;left:-5px", "position:fixed;bottom:0",
              "display:table", "display:table-cell", "display:none", "display:contents", "overflow:hidden;height:20px",
              "width:30%", "width:-10px", "margin:-20px", "padding:5px", "border:3px solid red", "white-space:pre",
              "font-size:0", "font-size:200px", "line-height:0", "columns:2", "flex-wrap:wrap", "z-index:-1;position:relative",
              "opacity:0.5", "vertical-align:middle", "text-align:justify", "word-break:break-all", "height:100%",
              "min-height:50vh", "max-width:0", "clear:both", "transform:rotate(3deg)", "list-style:inside square",
              "grid-column:1/-1", "flex:1 0 0", "order:-2", "letter-spacing:3px", "table-layout:fixed",
              "border-collapse:collapse", "content:'x'", "counter-increment:c"]
    words = ["lorem", "ipsum", "&amp;", "&nbsp;", "日本語", "x" * 80, "a-b-c", "­", "&#x200B;", "\t", "\n", "  "]
    out = []
    for _ in range(n):
        r = rnd.random()
        if r < 0.45:
            t = rnd.choice(tags)
            st = ";".join(rnd.choice(styles) for _ in range(rnd.randint(0, 3)))
            out.append("<%s style=\"%s\"%s>" % (t, st, rnd.choice(["", " colspan=3", " rowspan=0", " width=40 height=30",
                                                                       " id=x", " class=c", " href=#x", " type=checkbox"])))
        elif r < 0.7:
            out.append("</%s>" % rnd.choice(tags))
        else:
            out.append(" ".join(rnd.choice(words) for _ in range(rnd.randint(1, 6))))
    return ("<!DOCTYPE html><style>.c::before{content:counter(c) ' '}.c{counter-increment:c}"
            "p:hover{color:red} td:nth-child(2n){background:#eee}</style>" + "".join(out)).encode()


class Robustness(unittest.TestCase):
    """Fuzzing under AddressSanitizer: random and hostile pages through parse, style, layout, paint, hit test and
    find must not crash, and the per-document memory limit holds."""

    def test_fuzz(self):
        import random
        rnd = random.Random(int(os.environ.get("WEB_FUZZ_SEED", "1234")))
        count = int(os.environ.get("WEB_FUZZ_N", "150"))
        cmds = []
        for i in range(count):
            page = random_page(rnd, rnd.randint(5, 300))
            w = rnd.choice([1, 50, 320, 800, 1920])
            cmds.append((["layout", "w=%d" % w], page))
            if i % 3 == 0:
                cmds.append((["paint", "w=%d" % w], page))
                cmds.append((["hit", "w=%d" % w], b"%d %d\n" % (rnd.randint(0, 900), rnd.randint(0, 900)) + page))
                cmds.append((["find", "-"], b"lorem\n" + page))
        answers = run(cmds)
        self.assertEqual(len(answers), len(cmds))

    def test_deep_nesting_and_memory_limit(self):
        deep = b"<div>" * 100000 + b"x" + b"<span>" * 50000
        tables = b"<table><tr><td>" * 3000
        flexes = b"<div style='display:flex'><div style='display:grid'>" * 2000
        big = b"<p>" + b"word " * 400000
        got = run([(["layout", "-"], deep), (["layout", "-"], tables), (["layout", "-"], flexes),
                   (["layout", "limit=4000000"], big)], timeout=1800)
        self.assertTrue(all(g.endswith(b"\n") or g for g in got))
        self.assertIn(b"#status nomem", got[3])                # the limit stops it, gracefully


class Speed(unittest.TestCase):
    def test_one_megabyte(self):
        para = ("<p class=c>Lorem ipsum <b>dolor</b> sit amet, <a href='/x'>consectetur</a> adipiscing elit, sed do "
                "eiusmod tempor incididunt ut labore et dolore magna aliqua.</p>\n")
        rows = "<table>" + "".join("<tr><td>%d</td><td>cell text</td><td>more</td></tr>" % i for i in range(200)) + "</table>"
        doc = "<!DOCTYPE html><style>.c{margin:4px 0} td{padding:2px}</style>"
        while len(doc) < 1 << 20:
            doc += para * 20 + rows + "<div style='display:flex'>" + "<div style='flex:1'>flex item</div>" * 10 + "</div>"
        out = run([(["bench", "w=1024"], doc.encode())], fast=True)[0].decode()
        print("\n1 MB document (%s)" % out, file=sys.stderr)
        vals = dict(kv.split("=") for kv in out.split())
        self.assertLess(float(vals["parse"]) + float(vals["style"]) + float(vals["layout"]), 60000)


class SamplePages(unittest.TestCase):
    """The sample pages of tests/web/pages rendered to build/web-pages/*.ppm with the reference rasteriser."""

    def test_render(self):
        outdir = OUT.parent / "web-pages"
        outdir.mkdir(parents=True, exist_ok=True)
        pages = sorted((FIX / "pages").glob("*.html"))
        cmds = [(["render", "w=800,h=600,full,root=" + str(FIX / "pages")], (str(outdir / (p.stem + ".ppm")) + "\n").encode() +
                 p.read_bytes()) for p in pages]
        cmds += [(["layout", "w=800,root=" + str(FIX / "pages")], p.read_bytes()) for p in pages]
        got = [g.decode(errors="replace") for g in run(cmds)]
        for p, g in zip(pages, got):
            print("%s: %s" % (p.name, g), file=sys.stderr)
            self.assertIn("rc=0", g)
            self.assertGreater(int(re.search(r"items=(\d+)", g).group(1)), 20)
        lay = dict(zip([p.stem for p in pages], got[len(pages):]))
        boxes = {k: {l.split()[0]: l.split()[1:] for l in v.split("\n") if l.startswith("#") and len(l.split()) == 5}
                 for k, v in lay.items()}
        # the article's sidebar is beside the main column, not below it
        main, side = boxes["article"]["#main"], boxes["article"]["#side"]
        self.assertEqual(main[1], side[1])
        self.assertGreater(float(side[0]), float(main[0]) + float(main[2]))
        # the cards wrap into rows of three at 800px, and the form's controls are on their rows
        cards = boxes["cards"]
        self.assertEqual(cards["#card1"][1], cards["#card3"][1])
        self.assertGreater(float(cards["#card4"][1]), float(cards["#card1"][1]))
        self.assertIn("#stats", boxes["tables"])
        self.assertIn("#send", boxes["form"])


if __name__ == "__main__":
    unittest.main()
