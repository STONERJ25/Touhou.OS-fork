"""The json, re, csv, pickle, struct, copy, collections, dataclasses, enum, typing, functools, argparse, logging, configparser, pathlib, glob, tempfile and textual modules a program uses
to move text and structure around, with fixed inputs."""
import argparse
import collections
import configparser
import copy
import csv
import dataclasses
import enum
import fnmatch
import functools
import glob
import heapq
import bisect
import io
import json
import logging
import os
import pathlib
import pickle
import re
import shlex
import struct
import tempfile
import typing


def show(label, value=""):
    print(f"{label}: {value}")


print("--- json")
doc = {"name": "Reimu", "age": 17, "tags": ["shrine", "maiden"], "nested": {"a": [1, 2.5, None, True], "b": {}}, "unicode": "日本語 ☃ é", "float": 1e-7, "big": 2 ** 70}
text = json.dumps(doc)
show("dumps", text)
show("sorted and indented", json.dumps(doc, sort_keys=True, indent=2)[:120])
show("compact", json.dumps(doc, separators=(",", ":"), ensure_ascii=False)[:100])
show("round trip", json.loads(text) == doc)
show("loads", json.loads('[1, 2.0, "three", {"four": [5, null, false]}, 1e3, -0, 123456789012345678901234567890]'))
show("float repr", (json.dumps(0.1 + 0.2), json.dumps(1e22), json.dumps(1e-5), json.dumps(float("inf")), json.loads("NaN") != json.loads("NaN")))
for bad in ('{"a": }', "[1, 2,]", "{'single': 1}", '{"unterminated": "x', "", "tru"):
    try:
        json.loads(bad)
    except json.JSONDecodeError as e:
        show(f"error in {bad!r}", (e.msg, e.lineno, e.colno, e.pos))
show("escapes", (json.dumps("quote\" back\\ nl\n tab\t ctrl\x01   😀"), json.loads('"\\u00e9\\ud83d\\ude00\\n"')))
show("object_hook", json.loads('{"a": {"b": 1}}', object_hook=lambda d: sorted(d.items())))
show("default", json.dumps({"s": {1, 2}, "c": 3 + 4j}, default=lambda o: sorted(o) if isinstance(o, set) else str(o)))
show("keys", json.dumps({1: "int", 2.5: "float", True: "bool", None: "none"}))
buf = io.StringIO()
json.dump({"stream": [1, 2]}, buf)
show("dump/load", (buf.getvalue(), json.load(io.StringIO(buf.getvalue()))))
show("a large document", len(json.dumps([{"i": i, "s": str(i) * 3, "f": i / 7} for i in range(5000)])))
show("deep", len(json.dumps(json.loads("[" * 200 + "]" * 200))))

print("--- re")
show("search", [m.span() for m in re.finditer(r"\b\w{5}\b", "The quick brown foxes jumped over lazy dogs")])
show("groups", re.match(r"(?P<year>\d{4})-(?P<month>\d\d)-(?P<day>\d\d)", "2026-11-17").groupdict())
show("sub", (re.sub(r"(\w+) (\w+)", r"\2 \1", "hello world"), re.sub(r"\d+", lambda m: str(int(m.group()) * 2), "a1 b22 c333"), re.subn(r"o", "0", "foo boo")))
show("split", (re.split(r"[,;]\s*", "a, b;c,d"), re.split(r"(\d)", "a1b2c"), re.split(r"\s+", "  leading and trailing  ")))
show("flags", (re.findall(r"^\w+", "one\ntwo\nthree", re.M), re.findall(r"a.c", "a\nc abc", re.S), re.findall(r"HELLO", "Hello hello", re.I), re.findall(r"a b", "a b ab", re.X)))
show("greedy", (re.findall(r"<.+>", "<a><b>"), re.findall(r"<.+?>", "<a><b>"), re.findall(r"a{2,3}", "a aa aaa aaaa")))
show("lookaround", (re.findall(r"\w+(?=!)", "hi! there wow!"), re.findall(r"(?<=\$)\d+", "cost $15 or $20"), re.findall(r"\b(?!the)\w+", "the cat the dog")))
show("backrefs", (re.findall(r"(\w)\1", "aabbcd"), re.match(r"(['\"]).*?\1", "'quoted' rest").group()))
show("compile", (re.compile(r"\d+").pattern, re.compile(r"\d+", re.I).flags, re.escape("a.b*c"), re.fullmatch(r"\d+", "123") is not None, re.fullmatch(r"\d+", "123a")))
show("conditional", re.findall(r"(\()?\d+(?(1)\))", "(12) 34 (56"))
show("unicode classes", (re.findall(r"\w+", "日本語 abc ñ"), re.findall(r"[^\W\d_]+", "ab1_cd")))
show("bytes pattern", re.findall(rb"\d+", b"a1 b22"))
try:
    re.compile("(unclosed")
except re.error as e:
    show("re.error", (e.msg, e.pos))
show("catastrophic but bounded", re.match(r"(a+)+b", "a" * 22 + "b") is not None)
show("long text", len(re.findall(r"\b\w+\b", "word " * 100000)))
show("named group sub", re.sub(r"(?P<first>\w+) (?P<second>\w+)", r"\g<second> \g<first>", "one two three four"))

print("--- csv, struct, pickle, copy")
rows = [["name", "note"], ["Reimu", 'says "hi", twice'], ["日本", "line\nbreak"], ["", None]]
buf = io.StringIO(newline="")
csv.writer(buf).writerows(rows)
show("csv written", buf.getvalue())
show("csv read", list(csv.reader(io.StringIO(buf.getvalue(), newline=""))))
show("DictReader", list(csv.DictReader(io.StringIO("a,b\n1,2\n3,4\n"))))
show("struct", (struct.pack(">HIq", 1, 2, -3).hex(), struct.unpack("<3sBf", b"abc\x07\x00\x00\xc0\x3f"), struct.calcsize("<bhiq"), struct.calcsize("bhiq"), struct.pack("4s", b"ab"), list(struct.iter_unpack("<H", b"\x01\x00\x02\x00"))))
obj = {"list": [1, 2, {"deep": (3, 4)}], "set": {1, 2}, "bytes": b"\x00\xff", "float": 1.5, "complex": 1j, "none": None, "big": 2 ** 100, "str": "é"}
for proto in (0, 2, 4, 5):
    data = pickle.dumps(obj, protocol=proto)
    show(f"pickle protocol {proto}", (len(data), pickle.loads(data) == obj))
shared = [1, 2]
cyc = {"a": shared, "b": shared}
cyc["self"] = cyc
back = pickle.loads(pickle.dumps(cyc))
show("pickle keeps sharing and cycles", (back["a"] is back["b"], back["self"] is back))
show("copy", (copy.copy(obj)["list"] is obj["list"], copy.deepcopy(obj)["list"] is obj["list"], copy.deepcopy(cyc)["self"] is not cyc))
show("pickle of a class", pickle.loads(pickle.dumps(collections.OrderedDict(a=1, b=2))))

print("--- collections, dataclasses, enum, typing, functools")
c = collections.Counter("mississippi")
show("Counter", (c.most_common(3), c["s"], sorted(c.elements())[:4], (c - collections.Counter("miss")).most_common(2)))
d = collections.defaultdict(list)
for k, v in [("a", 1), ("b", 2), ("a", 3)]:
    d[k].append(v)
show("defaultdict", dict(d))
dq = collections.deque(range(5), maxlen=3)
dq.rotate(1)
show("deque", (list(dq), dq.maxlen))
P = collections.namedtuple("P", "x y")
show("namedtuple", (P(1, 2), P(1, 2)._replace(y=5), P._fields, P(3, 4)._asdict()))
show("ChainMap", collections.ChainMap({"a": 1}, {"a": 2, "b": 3})["b"])


@dataclasses.dataclass(order=True)
class Item:
    name: str
    price: float = 0.0
    tags: list = dataclasses.field(default_factory=list, compare=False)


show("dataclass", (Item("a", 2.5), Item("a") < Item("b"), dataclasses.asdict(Item("x", 1, ["t"])), dataclasses.replace(Item("a", 1), price=9), [f.name for f in dataclasses.fields(Item)]))


class Color(enum.Enum):
    RED = 1
    GREEN = 2


class Flag(enum.IntFlag):
    A = 1
    B = 2
    C = 4


show("enum", (Color.RED, Color(2), Color["GREEN"].value, list(Color), Flag.A | Flag.C, Flag(7), Flag.B in Flag.C))
show("typing", (typing.get_type_hints(Item)["price"], typing.Optional[int], typing.Union[int, str], typing.List[int]))


@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


show("lru_cache", (fib(80), fib.cache_info().hits, functools.reduce(lambda a, b: a * b, range(1, 21)), functools.partial(int, base=2)("1010")))
show("heapq/bisect", (heapq.nsmallest(3, [5, 1, 9, 3, 7]), heapq.nlargest(2, "hello"), bisect.bisect([1, 3, 5, 7], 4), sorted(heapq.merge([1, 4], [2, 3]))))

print("--- argparse, logging, configparser, shlex")
p = argparse.ArgumentParser(prog="tool", description="a tool")
p.add_argument("files", nargs="*")
p.add_argument("-v", "--verbose", action="count", default=0)
p.add_argument("-n", type=int, default=1)
p.add_argument("--mode", choices=["fast", "slow"], default="fast")
args = p.parse_args(["-vv", "-n", "5", "a.txt", "b.txt", "--mode", "slow"])
show("argparse", vars(args))
try:
    p.parse_args(["--mode", "bogus"])
except SystemExit as e:
    show("argparse error exits with", e.code)
show("usage", p.format_usage())
stream = io.StringIO()
logging.basicConfig(stream=stream, level=logging.DEBUG, format="%(levelname)s:%(name)s:%(message)s", force=True)
log = logging.getLogger("demo")
log.debug("debug %d", 1)
log.warning("careful")
logging.getLogger("demo.child").error("child error")
show("logging", stream.getvalue().splitlines())
cp = configparser.ConfigParser()
cp.read_string("[main]\nkey = value\nnumber = 42\n\n[other]\nflag = yes\n")
show("configparser", (cp["main"]["key"], cp.getint("main", "number"), cp.getboolean("other", "flag"), cp.sections()))
show("shlex", (shlex.split("a 'b c' \"d e\" f\\ g"), shlex.quote("it's here"), shlex.join(["a b", "c"])))

print("--- paths")
with tempfile.TemporaryDirectory() as tmp:
    base = pathlib.Path(tmp)
    (base / "sub" / "deeper").mkdir(parents=True)
    for rel in ("a.txt", "b.py", "sub/c.txt", "sub/deeper/d.py"):
        (base / rel).write_text(rel)
    show("glob", sorted(str(p.relative_to(base)) for p in base.glob("**/*.txt")))
    show("rglob", sorted(p.name for p in base.rglob("*.py")))
    show("iterdir", sorted(p.name for p in base.iterdir()))
    show("pathlib", ((base / "sub" / "c.txt").read_text(), (base / "a.txt").stat().st_size, (base / "sub").is_dir(), (base / "nope").exists(), (base / "a.txt").suffix, (base / "sub/c.txt").parent.name, (base / "a.txt").with_suffix(".md").name))
    show("glob module", sorted(os.path.relpath(p, tmp) for p in glob.glob(os.path.join(tmp, "*", "*.txt"))))
    show("tempfile names", (os.path.basename(tempfile.mkstemp(dir=tmp, prefix="pre_", suffix=".x")[1]).startswith("pre_"), os.path.exists(tempfile.mkdtemp(dir=tmp))))
    with tempfile.NamedTemporaryFile("w+", dir=tmp) as f:
        f.write("temp data")
        f.seek(0)
        show("NamedTemporaryFile", f.read())
    with tempfile.TemporaryFile() as f:
        f.write(b"anonymous")
        f.seek(0)
        show("TemporaryFile", f.read())
show("temp dir gone", os.path.exists(tmp))
show("fnmatch", (fnmatch.fnmatch("file.txt", "*.t?t"), fnmatch.filter(["a.py", "b.txt", "c.py"], "*.py"), fnmatch.translate("*.py")[:20]))
show("gettempdir", (tempfile.gettempdir(), tempfile.tempdir))
print("--- done")
