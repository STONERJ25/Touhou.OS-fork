"""vi: text objects and visual modes (characterwise, linewise, blockwise)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vilib import C, R, Qemu, ScriptRunner  # noqa: E402
from harness import test  # noqa: E402

OBJ_WORD = [
    R("diw", "foo bar baz\n", "wdiw", "foo  baz\n"),
    R("daw", "foo bar baz\n", "wdaw", "foo baz\n"),
    R("daw on the last word takes the blank before", "foo bar\n", "wdaw", "foo\n"),
    R("daw on the first word", "foo bar\n", "daw", "bar\n"),
    R("diw on blanks", "foo   bar\n", "3ldiw", "foobar\n"),
    R("daw on blanks", "foo   bar baz\n", "3ldaw", "foo baz\n"),
    R("d2aw", "a b c d\n", "d2aw", "c d\n"),
    R("d3iw", "foo bar baz\n", "d3iw", " baz\n"),
    R("diw on punctuation", "foo.bar\n", "3ldiw", "foobar\n"),
    R("diw in the middle of a word", "foobar\n", "lldiw", "\n"),
    R("ciw", "foo bar\n", "ciwX<Esc>", "X bar\n"),
    R("caw", "foo bar baz\n", "wcawX<Esc>", "foo Xbaz\n"),
    R("yiw p", "foo bar\n", "yiwwviwp", "foo foo\n"),
    R("diW", "a.b c\n", "diW", " c\n"),
    R("daW", "a.b c\n", "daW", "c\n"),
    R("diw on an empty line", "\nabc\n", "diw", "\nabc\n"),
    R("diw with UTF-8", "héllo wörld\n", "wdiw", "héllo \n"),
    R("gUiw", "foo bar\n", "wgUiw", "foo BAR\n"),
    R("g~iw", "foo bar\n", "g~iw", "FOO bar\n"),
    R(">ip", "a\nb\n\nc\n", ">ip", "\ta\n\tb\n\nc\n"),
]

OBJ_SENT_PARA = [
    R("dis", "One two. Three four. Five.\n", "fTdis", "One two.  Five.\n"),
    R("das", "One two. Three four. Five.\n", "fTdas", "One two. Five.\n"),
    R("das on the last sentence", "One two. Three four.\n", "fTdas", "One two.\n"),
    R("dip", "a\nb\n\nc\n", "dip", "\nc\n"),
    R("dap", "a\nb\n\nc\n", "dap", "c\n"),
    R("dap on the last paragraph takes the blank before", "a\n\nb\nc\n", "Gdap", "a\n"),
    R("dip on a blank paragraph", "a\n\n\nb\n", "jdip", "a\nb\n"),
    R("yip p", "a\nb\n\nc\n", "yipGp", "a\nb\n\nc\na\nb\n"),
    R("cip", "a\nb\n\nc\n", "cipX<Esc>", "X\n\nc\n"),
    R("d2ip", "a\n\nb\n\nc\n", "d2ip", "b\n\nc\n"),
    R("vip", "a\nb\n\nc\n", "vipd", "\nc\n"),
    R("vap", "a\nb\n\nc\n", "vapd", "c\n"),
]

OBJ_BLOCK = [
    R("di(", "f(a, b)\n", "fadi(", "f()\n"),
    R("da(", "f(a, b)\n", "fada(", "f\n"),
    R("dib", "f(a, b)\n", "fadib", "f()\n"),
    R("dab", "f(a, b)\n", "fadab", "f\n"),
    R("di) works too", "f(a, b)\n", "fadi)", "f()\n"),
    R("di{", "{ a }\n", "ldi{", "{}\n"),
    R("di{ over lines is linewise", "{\n  a\n}\n", "jdi{", "{\n}\n"),
    R("da{ over lines", "x {\n  a\n}\ny\n", "jda{", "x \ny\n"),
    R("diB", "{a}\n", "ldiB", "{}\n"),
    R("di[", "[a]\n", "ldi[", "[]\n"),
    R("da[", "x[a]\n", "lda[", "x\n"),
    R("di<", "<a>\n", "ldi<", "<>\n"),
    R("ci(", "(a)\n", "lci(X<Esc>", "(X)\n"),
    R("d2i(", "((a))\n", "lld2i(", "()\n"),
    R("di( on the open paren", "(a)\n", "di(", "()\n"),
    R("di( on the close paren", "(a)\n", "$di(", "()\n"),
    R("di( not found", "abc\n", "di(", "abc\n"),
    R("di( skips nested pairs", "(a (b) c)\n", "ldi(", "()\n"),
    R("di( inside nested pairs", "(a (b) c)\n", "fbdi(", "(a () c)\n"),
    R("yi( p", "(ab)\n", "lyi($p", "(ab)ab\n"),
    R("di( over lines keeps the indent lines", "f(\n  a,\n  b\n)\n", "jdi(", "f(\n)\n"),
    R("ci{ over lines", "{\n  a\n}\n", "jci{X<Esc>", "{\nX\n}\n"),
]

OBJ_QUOTE_TAG = [
    R('di"', 'say "hi there" now\n', 'fhdi"', 'say "" now\n'),
    R('da"', 'say "hi there" now\n', 'fhda"', "say now\n"),
    R('di" before the first quote', 'say "hi"\n', 'di"', 'say ""\n'),
    R('da" at the end takes the blank before', 'say "hi"\n', 'fhda"', "say\n"),
    R("di'", "it's 'ok' now\n", "fodi'", "it's '' now\n"),
    R("di`", "run `ls` now\n", "flldi`", "run `` now\n"),
    R('ci"', '"a"\n', 'lci"X<Esc>', '"X"\n'),
    R('di" with an escaped quote', '"a\\"b" c\n', 'ldi"', '"" c\n'),
    R('di" on an empty string', 'x "" y\n', 'f"di"', 'x "" y\n'),
    R('yi" p', 'x "ab" y\n', 'fayi"$p', 'x "ab" yab\n'),
    R("dit", "<a><b>x</b></a>\n", "fxdit", "<a><b></b></a>\n"),
    R("dat", "<a><b>x</b></a>\n", "fxdat", "<a></a>\n"),
    R("d2it", "<a><b>x</b></a>\n", "fxd2it", "<a></a>\n"),
    R("cit on the opening tag", "<p>hi</p>\n", "citX<Esc>", "<p>X</p>\n"),
    R("dit over lines", "<a>\nx\n</a>\n", "jdit", "<a></a>\n"),
    R("dit with attributes", '<a href="x">t</a>\n', "ftdit", '<a href="x"></a>\n'),
    R("dit skips a different tag", "<a>x<br/>y</a>\n", "fxdit", "<a></a>\n"),
    R("dat on the last tag", "<a>x</a>\n", "fxdat", "\n"),
]

VISUAL = [
    R("v d", "abcdef\n", "lvld", "adef\n"),
    R("v w", "foo bar baz\n", "vwd", "ar baz\n"),
    R("v e", "foo bar\n", "ved", " bar\n"),
    R("v with a count", "abcdef\n", "v3ld", "ef\n"),
    R("v y p", "abc\n", "vly$p", "abcab\n"),
    R("v c", "abcd\n", "vlcX<Esc>", "Xcd\n"),
    R("v $ takes the newline", "abc\ndef\n", "lv$d", "adef\n"),
    R("v o swaps ends", "abcdef\n", "llvlohd", "aef\n"),
    R("v iw", "foo bar\n", "wviwd", "foo \n"),
    R("v aw", "foo bar baz\n", "wvawd", "foo baz\n"),
    R("v iw twice extends", "foo bar baz\n", "viwiwd", "bar baz\n"),
    R("v i(", "f(a, b)\n", "favi(d", "f()\n"),
    R("v a(", "f(a, b)\n", "fava(d", "f\n"),
    R("v i( twice extends to the outer pair", "((ab))\n", "llvi(i(d", "()\n"),
    R("v i( on a one-character block stays", "((a))\n", "llvi(i(d", "(())\n"),
    R("v i\"", 'x "ab" y\n', 'favi"d', 'x "" y\n'),
    R("v %", "(a)b\n", "v%d", "b\n"),
    R("v search", "foo bar baz\n", "v/baz<CR>d", "az\n"),
    R("v j", "ab\ncd\nef\n", "vjd", "d\nef\n"),
    R("v u", "ABC\n", "vlu", "abC\n"),
    R("v U", "abc\n", "vlU", "ABc\n"),
    R("v ~", "aBc\n", "vl~", "Abc\n"),
    R("v g?", "abc\n", "vlg?", "noc\n"),
    R("v r", "abcd\n", "vlrX", "XXcd\n"),
    R("v J", "a\nb\nc\n", "vjjJ", "a b c\n"),
    R("v gJ", "a\n b\n", "vjgJ", "a b\n"),
    R("v >", "a\nb\nc\n", "vj>", "\ta\n\tb\nc\n"),
    R("v 2>", "a\n", "v2>", "\t\ta\n"),
    R("v <", "\ta\n\tb\n", "vj<", "a\nb\n"),
    R("v p replaces the selection", "foo bar\n", "yiwwviwp", "foo foo\n"),
    R("v p swaps the unnamed register", "foo bar\n", "yiwwviwp0viwp", "bar foo\n"),
    R("v x", "abc\n", "vlx", "c\n"),
    R("v s", "abcd\n", "vlsX<Esc>", "Xcd\n"),
    R("v D deletes the lines", "abc\ndef\n", "lvD", "def\n"),
    R("v X deletes the lines", "abc\ndef\n", "lvX", "def\n"),
    R("v C changes the lines", "abc\ndef\n", "lvCX<Esc>", "X\ndef\n"),
    R("v Y yanks the lines", "a\nb\n", "vYjp", "a\nb\na\n"),
    R("v : fills the range", "a\nb\nc\n", "vj:d<CR>", "c\n"),
    R("v : with a substitute", "a\na\na\n", "Vj:s/a/b/<CR>", "b\nb\na\n"),
    R("gv reselects", "abcdef\n", "vly<Esc>gvd", "cdef\n"),
    R("gv after another selection", "abcdef\n", "vly$vygvd", "abcde\n"),
    R("v then V", "abc\ndef\n", "vjVd", ""),
    R("v then Esc", "abc\n", "vl<Esc>x", "ac\n"),
    R("v v leaves visual mode", "abc\n", "vlvx", "ac\n"),
    R("V d", "a\nb\nc\n", "Vjd", "c\n"),
    R("V y p", "a\nb\n", "Vyjp", "a\nb\na\n"),
    R("V c", "a\nb\nc\n", "VjcX<Esc>", "X\nc\n"),
    R("V p", "a\nb\n", "yyjVp", "a\na\n"),
    R("V with G", "1\n2\n3\n", "VGd", ""),
    R("V ip", "a\nb\n\nc\n", "Vipd", "\nc\n"),
    R("v 'x with a mark", "a\nb\nc\nd\n", "majjmb'aV'bd", "d\n"),
    R("v with o and a longer move", "abcdef\n", "llvlohhd", "ef\n"),
]

BLOCK = [
    R("^V d", "abc\ndef\nghi\n", "l<C-v>jld", "a\nd\nghi\n"),
    R("^V I", "abc\ndef\nghi\n", "<C-v>jjIX<Esc>", "Xabc\nXdef\nXghi\n"),
    R("^V I in the middle", "abc\ndef\nghi\n", "l<C-v>jjIX<Esc>", "aXbc\ndXef\ngXhi\n"),
    R("^V A", "abc\ndef\nghi\n", "<C-v>jjAX<Esc>", "aXbc\ndXef\ngXhi\n"),
    R("^V $ A", "a\nbcd\ne\n", "<C-v>jj$AX<Esc>", "aX\nbcdX\neX\n"),
    R("^V c", "abc\ndef\n", "l<C-v>jcX<Esc>", "aXc\ndXf\n"),
    R("^V r", "abc\ndef\n", "l<C-v>jrX", "aXc\ndXf\n"),
    R("^V y $ p", "ab\ncd\n", "<C-v>jy$p", "aba\ncdc\n"),
    R("^V y P", "ab\ncd\n", "<C-v>jylP", "aab\nccd\n"),
    R("^V with a short line", "abc\nd\nefg\n", "ll<C-v>jjd", "ab\nd\nef\n"),
    R("^V I leaves short lines alone", "abc\nd\nefg\n", "ll<C-v>jjIX<Esc>", "abXc\nd\nefXg\n"),
    R("^V A pads short lines", "abc\nd\nefg\n", "ll<C-v>jjAX<Esc>", "abcX\nd  X\nefgX\n"),
    R("^V o O", "abcd\nefgh\n", "l<C-v>jlOd", "ad\neh\n"),
    R("^V U", "abc\ndef\n", "<C-v>jlU", "ABc\nDEf\n"),
    R("^V x", "abc\ndef\n", "<C-v>jx", "bc\nef\n"),
    R("^V with a count", "abcd\nefgh\n", "<C-v>j2lx", "d\nh\n"),
    R("^V I with multiple characters", "ab\ncd\n", "<C-v>jIXY<Esc>", "XYab\nXYcd\n"),
    R("^V u", "ABC\nDEF\n", "<C-v>jlu", "abC\ndeF\n"),
]


def run(name, *tables):
    with Qemu(cmdline="text", name=f"t-vi-{name}") as vm:
        sr = ScriptRunner(vm)
        for t in tables:
            sr.run_rows(t)
        sr.finish(name)


@test
def vi_textobjects():
    """vi: text objects (words, sentences, paragraphs, brackets, quotes, tags) with operators."""
    run("textobjects", OBJ_WORD, OBJ_SENT_PARA, OBJ_BLOCK, OBJ_QUOTE_TAG)


@test
def vi_visual_modes():
    """vi: visual mode (characters, lines, blocks) with operators, text objects and ranges."""
    run("visual", VISUAL, BLOCK)
