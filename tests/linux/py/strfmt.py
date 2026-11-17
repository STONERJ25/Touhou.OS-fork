"""Strings: methods, str.format, f-strings and % formatting, unicode (case mapping, normalization, categories, names), encodings and their errors, bytes, text wrapping, string
tables, io streams and the locale of the C.UTF-8 environment."""
import codecs
import io
import locale
import re
import string
import sys
import textwrap
import unicodedata


def show(label, value=""):
    print(f"{label}: {value}")


print("--- methods")
s = "  The Quick Brown Fox  "
show("strip/case", (s.strip(), s.lstrip(), s.rstrip(), s.upper(), s.lower(), s.title(), s.swapcase(), s.strip().capitalize(), "ß".upper(), "İ".lower(), "ǆ".title()))
show("split", ("a,b,,c".split(","), "a b  c".split(), "a,b,c".rsplit(",", 1), "line1\nline2\r\nline3".splitlines(), "a=b=c".partition("="), "a=b=c".rpartition("=")))
show("find", ("hello".find("l"), "hello".rfind("l"), "hello".find("z"), "hello".index("e"), "hello".count("l"), "hello".startswith(("he", "x")), "hello".endswith("lo")))
show("replace/translate", ("aaa".replace("a", "b", 2), "hello".translate(str.maketrans("el", "ip")), "hello".translate({ord("l"): None}), "tab\there".expandtabs(4)))
show("justify", ("x".center(7, "*"), "x".ljust(4, "."), "x".rjust(4, "."), "42".zfill(6), "-42".zfill(6)))
show("predicates", ("abc".isalpha(), "abc1".isalnum(), "123".isdigit(), "²".isdigit(), "²".isdecimal(), "²".isnumeric(), " ".isspace(), "Abc".istitle(), "ABC".isupper(), "abc".isidentifier(), "ǅ".istitle(), "٣".isdecimal()))
show("join/repeat", ("-".join("abc"), "ab" * 3, "".join(reversed("stressed")), "a" + "b" * 2))
show("compare", ("a" < "b", "B" < "a", "é" > "z", "abc" < "abd", "10" < "9", sorted(["b", "A", "a", "B", "é", "z"])))
show("casefold", ("Straße".casefold() == "STRASSE".casefold(), "ΑΣ".lower(), "ΑΣ ".lower()))
show("removeprefix", ("test_case".removeprefix("test_"), "file.py".removesuffix(".py")))
show("len and slicing", (len("héllo"), len("日本語"), "日本語"[1], "hello"[::-1], "hello"[1:4], "hello"[-3:], "hello"[::2], len("🎌"), len("é")))

print("--- formatting")
name, n, x = "Reimu", 42, 3.14159265
show("f-strings", (f"{name} {n} {x:.2f}", f"{name!r:>10}", f"{n:05d}", f"{n:x} {n:#b}", f"{x:10.3e}|", f"{n=}", f"{'nested ' + name}", f"{{literal}}", f"{n:,}", f"{0.256:.1%}"))
show("format spec", ("{:>8}|{:<8}|{:^8}|".format("r", "l", "c"), "{:*^9}".format("mid"), "{0} {1} {0}".format("a", "b"), "{k}={v}".format(k="key", v=1), "{:+.3f}".format(2.5), "{:08.3f}".format(-2.5)))
show("format types", ("{:d} {:o} {:x} {:X} {:b} {:c}".format(65, 65, 255, 255, 5, 65), "{:e} {:g} {:G}".format(12345.6789, 12345.6789, 1e-10), "{:s}".format("text"), "{:%}".format(0.5)))
show("format attributes", ("{0.real} {0.imag}".format(3 + 4j), "{0[1]} {0[0]}".format([10, 20]), "{p[name]}".format(p={"name": "dict"})))
show("percent", ("%d items, %5.2f%%, %s, %r, %x, %o, %e, %c, %-6d|, %06d" % (3, 99.5, "str", "repr", 255, 8, 1234.5, 65, 7, 42), "%(a)s-%(b)s" % {"a": 1, "b": 2}, "%*d" % (6, 42), "%.3s" % "truncated"))
show("repr/ascii", (repr("quote'dq\"nl\ntab\t\x00é日"), ascii("é日"), repr(b"bytes\xff\n"), str(b"x"), repr("\U0001F38C")))
show("templates", (string.Template("$who likes ${what}s").substitute(who="Marisa", what="mushroom"), string.Template("$a $b").safe_substitute(a=1)))
show("string module", (string.ascii_lowercase[:5], string.digits, string.capwords("hello big  world"), string.punctuation[:8], len(string.printable)))
show("round trip of floats", [float(repr(v)) == v for v in (0.1, 1e300, 5e-324, 123456.789)])
show("large format", len(f"{2 ** 1000:,}"))
for width in (1, 5, 10, 40):
    show(f"column {width}", f"{'abcdefghij' * 5:.{width}}|{'x':>{width}}|")

print("--- unicode")
text = "Café é Å Å ﬁ Ⅰ Ａ"
for form in ("NFC", "NFD", "NFKC", "NFKD"):
    t = unicodedata.normalize(form, text)
    show(form, (len(t), [hex(ord(c)) for c in t][:8]))
show("category/name", [(c, unicodedata.category(c), unicodedata.name(c, "?")) for c in "aZ5 \n日é😀́"])
show("lookup", (unicodedata.lookup("SNOWMAN"), unicodedata.name("☃"), unicodedata.numeric("½"), unicodedata.decimal("٣"), unicodedata.digit("²"), unicodedata.bidirectional("א"), unicodedata.east_asian_width("日"), unicodedata.combining("́")))
show("version", unicodedata.unidata_version)
show("case mapping", ("ǉ".upper(), "ﬁ".upper(), "ŉ".upper(), "İ".lower().encode("utf-8"), "ꭰ".upper(), "Σ".lower(), "ς".upper()))
show("sorting by code point", sorted("zäaÄéE日"))
show("surrogates", ("\ud800" in "a\ud800b", "\U0001F600".encode("utf-16-le"), len("\U0001F600".encode("utf-16-le")), "\U0001F600".encode("utf-16").hex()))
show("regex unicode", (re.findall(r"\w+", "naïve café 日本語 ünï"), re.findall(r"\d+", "١٢٣ 456"), re.sub(r"\s+", " ", "a b c"), re.match(r"(?i)straße", "STRASSE") is not None))

print("--- encodings")
s = "héllo wörld ☃ 日本語 😀"
for enc in ("utf-8", "utf-16", "utf-16-be", "utf-32-le", "latin-1", "ascii", "cp1252", "cp437", "iso-8859-15", "shift_jis", "euc-jp", "gbk", "big5", "koi8-r", "utf-7", "idna", "punycode"):
    try:
        b = s.encode(enc)
        show(enc, (len(b), b[:12].hex(), b.decode(enc) == s))
    except (UnicodeError, LookupError) as e:
        show(enc, type(e).__name__)
show("errors=", (s.encode("ascii", "replace"), s.encode("ascii", "ignore"), s.encode("ascii", "xmlcharrefreplace"), s.encode("ascii", "backslashreplace"), s.encode("ascii", "namereplace")[:60]))
show("decode errors", (b"caf\xe9 ok".decode("utf-8", "replace"), b"caf\xe9".decode("utf-8", "ignore"), b"caf\xe9".decode("latin-1"), b"\xff\xfe".decode("utf-8", "backslashreplace"), b"a\x80b".decode("utf-8", "surrogateescape").encode("utf-8", "surrogateescape")))
try:
    b"\xff".decode("utf-8")
except UnicodeDecodeError as e:
    show("UnicodeDecodeError", (e.encoding, e.reason, e.start, e.end, e.object))
try:
    "日".encode("latin-1")
except UnicodeEncodeError as e:
    show("UnicodeEncodeError", (e.encoding, e.reason, e.start))
show("codecs", (codecs.encode("hello", "rot13"), codecs.encode(b"hello", "hex"), codecs.decode("68656c6c6f", "hex"), codecs.encode(b"hello", "base64"), codecs.lookup("utf8").name, codecs.lookup("latin1").name))
show("BOM", (codecs.BOM_UTF8, b"\xef\xbb\xbfdata".decode("utf-8-sig"), "x".encode("utf-8-sig")))
dec = codecs.getincrementaldecoder("utf-8")()
show("incremental decoder", [dec.decode(b) for b in (b"\xe6", b"\x97", b"\xa5", b" ok")])
show("bytes", (bytes([72, 105]), b"abc".hex(), bytes.fromhex("48 69"), b"a,b".split(b","), b"abc".upper(), bytearray(b"abc") + b"d", b"%d-%s" % (5, b"x"), memoryview(b"hello")[1:3].tobytes()))
show("bytes vs str", (b"a" == "a", b"abc"[1], b"abc"[1:2], bytes("é", "utf-8"), str(b"\xc3\xa9", "utf-8")))

print("--- streams and text")
buf = io.StringIO()
print("to a StringIO", file=buf, end="!")
print(1, 2, 3, sep="-", file=buf)
show("StringIO", buf.getvalue())
b = io.BytesIO(b"line one\nline two\r\nline three")
show("BytesIO", (b.readline(), b.read(5), b.tell(), b.seek(0), b.readlines()))
t = io.TextIOWrapper(io.BytesIO("héllo\nwörld\r\nthird".encode("utf-8")), encoding="utf-8", newline=None)
show("universal newlines", t.readlines())
t = io.TextIOWrapper(io.BytesIO(), encoding="latin-1", newline="\r\n")
t.write("a\nb\n")
t.flush()
show("newline translation", t.buffer.getvalue())
show("stdout", (sys.stdout.encoding, sys.stdout.errors, sys.stdout.line_buffering, sys.stdin.encoding, sys.stderr.errors))
show("locale", (locale.getpreferredencoding(False), locale.getlocale(), locale.setlocale(locale.LC_ALL, None)))
show("wrap", textwrap.wrap("The quick brown fox jumps over the lazy dog " * 3, width=30))
show("fill/indent/dedent", (textwrap.fill("a b c d e f", width=5), textwrap.indent("one\ntwo", "> "), textwrap.dedent("    x\n      y\n    z"), textwrap.shorten("The quick brown fox jumps over", width=18, placeholder="…")))
words = "the quick brown fox jumps over the lazy dog the end".split()
counts = {}
for w in words:
    counts[w] = counts.get(w, 0) + 1
show("word counts", sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))
rows = [("name", "qty", "price"), ("apple", 3, 1.5), ("banana", 12, 0.25), ("cherry", 100, 10.0)]
for r in rows:
    print(f"{r[0]:<10}{r[1]:>5}{r[2]:>9}")
print("日本語".ljust(8, "-") + "|", "日本語".center(9, "*"), sep=" ")
print("emoji 😀 and combining é and right-to-left אב print without trouble")
print("--- done")
