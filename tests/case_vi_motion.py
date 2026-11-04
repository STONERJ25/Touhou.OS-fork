"""vi: cursor motions with counts, searches and marks. Every row starts `vi -s keys file` (see vilib.py); `|` marks the cursor
character before and after the keys."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vilib import C, R, Qemu, ScriptRunner  # noqa: E402
from harness import test  # noqa: E402


def numbered(n, mark=None, prefix=""):
    """'1\\n2\\n...n\\n' with a cursor marker in front of line `mark`."""
    return "".join(("|" if i == mark else "") + prefix + str(i) + "\n" for i in range(1, n + 1))


CHAR_LINE = [
    C("h", "ab|cd\n", "h", "a|bcd\n"),
    C("2h", "abc|de\n", "2h", "a|bcde\n"),
    C("h at column 0", "|abc\n", "h", "|abc\n"),
    C("9h clamps", "ab|cd\n", "9h", "|abcd\n"),
    C("l", "|abc\n", "l", "a|bc\n"),
    C("3l", "|abcde\n", "3l", "abc|de\n"),
    C("l at the last character", "ab|c\n", "l", "ab|c\n"),
    C("9l clamps", "|abc\n", "9l", "ab|c\n"),
    C("Space wraps to the next line", "ab|c\ndef\n", "<Space>", "abc\n|def\n"),
    C("4 Space stops at the end of the buffer", "|ab\ncd\n", "4<Space>", "ab\nc|d\n"),
    C("Backspace", "ab|c\n", "<BS>", "a|bc\n"),
    C("Backspace wraps to the previous line", "abc\n|def\n", "<BS>", "ab|c\ndef\n"),
    C("arrow Left", "ab|c\n", "<Left>", "a|bc\n"),
    C("arrow Right", "a|bc\n", "<Right>", "ab|c\n"),
    C("j", "ab|c\ndef\n", "j", "abc\nde|f\n"),
    C("j keeps the column over a short line", "abc|d\nx\nfghi\n", "2j", "abcd\nx\nfgh|i\n"),
    C("k", "abc\nd|ef\n", "k", "a|bc\ndef\n"),
    C("k keeps the column over a short line", "abcd\nx\nfgh|i\n", "2k", "abc|d\nx\nfghi\n"),
    C("3j", "|a\nb\nc\nd\ne\n", "3j", "a\nb\nc\n|d\ne\n"),
    C("j on the last line", "a\n|b\n", "j", "a\n|b\n"),
    C("5j clamps to the last line", "|a\nb\nc\n", "5j", "a\nb\n|c\n"),
    C("9k clamps to the first line", "a\nb\n|c\n", "9k", "|a\nb\nc\n"),
    C("arrow Down", "|a\nb\n", "<Down>", "a\n|b\n"),
    C("arrow Up", "a\n|b\n", "<Up>", "|a\nb\n"),
    C("+", "|a\n  b\n", "+", "a\n  |b\n"),
    C("2+", "|a\nb\n  c\n", "2+", "a\nb\n  |c\n"),
    C("-", "  a\n|b\n", "-", "  |a\nb\n"),
    C("Enter", "|a\n  b\n", "<CR>", "a\n  |b\n"),
    C("_", "  a|b\n", "_", "  |ab\n"),
    C("2_", "|a\n  b\nc\n", "2_", "a\n  |b\nc\n"),
    C("0", "ab|cd\n", "0", "|abcd\n"),
    C("^", "  ab|cd\n", "^", "  |abcd\n"),
    C("^ on a blank line", "|   \n", "^", "  | \n"),
    C("$", "|abcd\n", "$", "abc|d\n"),
    C("2$", "|ab\ncd\nef\n", "2$", "ab\nc|d\nef\n"),
    C("$ on an empty line", "a\n|\nb\n", "$", "a\n|\nb\n"),
    C("$ then j keeps the end", "|abcd\nef\nghijk\n", "$jj", "abcd\nef\nghij|k\n"),
    C("g_", "|ab  \n", "g_", "a|b  \n"),
    C("2g_", "|ab\ncd  \n", "2g_", "ab\nc|d  \n"),
    C("|", "|abcdef\n", "4|", "abc|def\n"),
    C("| past the end", "|abc\n", "20|", "ab|c\n"),
    C("| counts display columns", "|\tab\n", "9|", "\t|ab\n"),
    C("G", "|a\nb\nc\n", "G", "a\nb\n|c\n"),
    C("G goes to the first non-blank", "|a\nb\n  c\n", "G", "a\nb\n  |c\n"),
    C("2G", "|a\nb\nc\n", "2G", "a\n|b\nc\n"),
    C("99G clamps", "|a\nb\nc\n", "99G", "a\nb\n|c\n"),
    C("gg", "a\nb\n|c\n", "gg", "|a\nb\nc\n"),
    C("2gg", "|a\nb\nc\n", "2gg", "a\n|b\nc\n"),
    C("1G", "a\nb\n|c\n", "1G", "|a\nb\nc\n"),
    C("50%", "|a\nb\nc\nd\n", "50%", "a\n|b\nc\nd\n"),
    C("100%", "|a\nb\nc\nd\n", "100%", "a\nb\nc\n|d\n"),
    C("gm", "|abc\n", "gm", "ab|c\n"),
    C("go", "|ab\ncd\n", "4go", "ab\n|cd\n"),
]

WORDS = [
    C("w", "|foo bar\n", "w", "foo |bar\n"),
    C("w stops at punctuation", "|foo.bar\n", "w", "foo|.bar\n"),
    C("ww over punctuation", "|foo.bar\n", "ww", "foo.|bar\n"),
    C("w to the next line", "fo|o\nbar\n", "w", "foo\n|bar\n"),
    C("w skips leading blanks on the next line", "fo|o\n  bar\n", "w", "foo\n  |bar\n"),
    C("w stops at an empty line", "|a\n\nb\n", "w", "a\n|\nb\n"),
    C("w twice over an empty line", "|a\n\nb\n", "ww", "a\n\n|b\n"),
    C("3w", "|a b c d\n", "3w", "a b c |d\n"),
    C("w on the last word of the file", "foo |bar\n", "w", "foo ba|r\n"),
    C("w with UTF-8 letters", "|héllo wörld\n", "w", "héllo |wörld\n"),
    C("b", "foo |bar\n", "b", "|foo bar\n"),
    C("b inside a word", "foo ba|r\n", "b", "foo |bar\n"),
    C("b to the previous line", "foo\n|bar\n", "b", "|foo\nbar\n"),
    C("2b", "a b c |d\n", "2b", "a |b c d\n"),
    C("b over punctuation", "foo.|bar\n", "b", "foo|.bar\n"),
    C("b over an empty line", "a\n\n|b\n", "b", "a\n|\nb\n"),
    C("b at the start of the file", "|foo\n", "b", "|foo\n"),
    C("e", "|foo bar\n", "e", "fo|o bar\n"),
    C("e again goes to the next word end", "fo|o bar\n", "e", "foo ba|r\n"),
    C("e over punctuation", "|foo.bar\n", "e", "fo|o.bar\n"),
    C("2e", "|a bc de\n", "2e", "a bc d|e\n"),
    C("e to the next line", "fo|o\nbar\n", "e", "foo\nba|r\n"),
    C("ge", "foo ba|r\n", "ge", "fo|o bar\n"),
    C("ge from the start of a word", "foo |bar\n", "ge", "fo|o bar\n"),
    C("gE", "a.b c.|d\n", "gE", "a.|b c.d\n"),
    C("W", "|a.b c.d\n", "W", "a.b |c.d\n"),
    C("B", "a.b c.|d\n", "B", "a.b |c.d\n"),
    C("E", "|a.b c.d\n", "E", "a.|b c.d\n"),
    C("2W", "|a.b c.d e\n", "2W", "a.b c.d |e\n"),
]

FIND = [
    C("f", "|a,b,c\n", "f,", "a|,b,c\n"),
    C("2f", "|a,b,c\n", "2f,", "a,b|,c\n"),
    C("f not found", "|abc\n", "fx", "|abc\n"),
    C("3f too many", "|a,b,c\n", "3f,", "|a,b,c\n"),
    C("F", "a,b,|c\n", "F,", "a,b|,c\n"),
    C("2F", "a,b,|c\n", "2F,", "a|,b,c\n"),
    C("t", "|ab,c\n", "t,", "a|b,c\n"),
    C("T", "a,bc|d\n", "T,", "a,|bcd\n"),
    C("; repeats f", "|a,b,c,d\n", "f,;", "a,b|,c,d\n"),
    C(", reverses", "|a,b,c,d\n", "3f,,", "a,b|,c,d\n"),
    C("; after t does not get stuck", "|ab,cd,ef\n", "t,;", "ab,c|d,ef\n"),
    C("2; ", "|a,b,c,d\n", "f,2;", "a,b,c|,d\n"),
    C("f with a UTF-8 target", "|abéc\n", "fé", "ab|éc\n"),
    C("f over UTF-8 text", "|aéb\n", "fb", "aé|b\n"),
]

PARA_SENT = [
    C("}", "|a\nb\n\nc\n", "}", "a\nb\n|\nc\n"),
    C("2}", "|a\n\nb\n\nc\n", "2}", "a\n\nb\n|\nc\n"),
    C("} without a blank line goes to the end", "|a\nb\n", "}", "a\n|b\n"),
    C("} from a blank line", "|\na\n\nb\n", "}", "\na\n|\nb\n"),
    C("{", "a\n\nb\n|c\n", "{", "a\n|\nb\nc\n"),
    C("{ to the start", "a\n|b\n", "{", "|a\nb\n"),
    C(")", "|One two. Three four. Five.\n", ")", "One two. |Three four. Five.\n"),
    C("2)", "|One two. Three four. Five.\n", "2)", "One two. Three four. |Five.\n"),
    C(") to the next line", "|A b.\nC d.\n", ")", "A b.\n|C d.\n"),
    C(") stops at an empty line", "|A b\n\nC d\n", ")", "A b\n|\nC d\n"),
    C(") with ! and ?", "|Hi! Ok? Yes.\n", "2)", "Hi! Ok? |Yes.\n"),
    C("(", "One two. Three |four. Five.\n", "(", "One two. |Three four. Five.\n"),
    C("( at a sentence start goes to the previous", "One two. |Three four.\n", "(", "|One two. Three four.\n"),
    C("2(", "One. Two. Th|ree.\n", "2(", "One. |Two. Three.\n"),
]

BRACKETS = [
    C("%", "|(a [b] c)\n", "%", "(a [b] c|)\n"),
    C("% back", "(a [b] c|)\n", "%", "|(a [b] c)\n"),
    C("% on an inner bracket", "(a |[b] c)\n", "%", "(a [b|] c)\n"),
    C("% scans forward for a bracket", "|x (a)\n", "%", "x (a|)\n"),
    C("% over lines", "|{\na\n}\n", "%", "{\na\n|}\n"),
    C("% nested", "|((a)(b))\n", "%", "((a)(b)|)\n"),
    C("% braces and brackets", "|{[}]}\n", "%", "{[|}]}\n"),
    C("% without a bracket", "|abc\n", "%", "|abc\n"),
    C("[(", "(a (b |c) d)\n", "[(", "(a |(b c) d)\n"),
    C("2[(", "(a (b |c) d)\n", "2[(", "|(a (b c) d)\n"),
    C("])", "(a (b |c) d)\n", "])", "(a (b c|) d)\n"),
    C("[{", "{ a { b |c } d }\n", "[{", "{ a |{ b c } d }\n"),
    C("]}", "{ a { b |c } d }\n", "]}", "{ a { b c |} d }\n"),
    C("]]", "|x\n{\ny\n}\n", "]]", "x\n|{\ny\n}\n"),
    C("[[", "x\n{\ny\n|}\n", "[[", "x\n|{\ny\n}\n"),
    C("][", "|x\n{\ny\n}\n", "][", "x\n{\ny\n|}\n"),
    C("[]", "x\n{\ny\n}\nz\n|w\n", "[]", "x\n{\ny\n|}\nz\nw\n"),
]

SEARCH = [
    C("/", "|abc\nfoo\nbar foo\n", "/foo<CR>", "abc\n|foo\nbar foo\n"),
    C("/ then n", "|abc\nfoo\nbar foo\n", "/foo<CR>n", "abc\nfoo\nbar |foo\n"),
    C("n wraps around", "|abc\nfoo\nbar foo\n", "/foo<CR>nn", "abc\n|foo\nbar foo\n"),
    C("N goes back (wrapping)", "|abc\nfoo\nbar foo\n", "/foo<CR>N", "abc\nfoo\nbar |foo\n"),
    C("?", "abc\nfoo\nbar f|oo\n", "?foo<CR>", "abc\nfoo\nbar |foo\n"),
    C("? then n goes on backwards", "foo\nfoo\nf|oo\n", "?foo<CR>n", "foo\n|foo\nfoo\n"),
    C("? then N goes forward (wrapping)", "foo\nfoo\nf|oo\n", "?foo<CR>N", "|foo\nfoo\nfoo\n"),
    C("3/", "|a1\na2\na3\na4\n", "3/a<CR>", "a1\na2\na3\n|a4\n"),
    C("/ with an empty pattern repeats", "|foo\nbar\nfoo\nbaz\nfoo\n", "/foo<CR>/<CR>", "foo\nbar\nfoo\nbaz\n|foo\n"),
    C("/ finds a match later on the same line", "|ab ab\n", "/ab<CR>", "ab |ab\n"),
    C("/ not found keeps the cursor", "|abc\n", "/xyz<CR>", "|abc\n"),
    C("/ pattern with a regex", "|a\nb12\nc\n", "/[0-9]\\+<CR>", "a\nb|12\nc\n"),
    C("/ ignores case with \\c", "|a\nFOO\n", "/foo\\c<CR>", "a\n|FOO\n"),
    C("/ with ^ and $", "|ab\nxab\nab\n", "/^ab$<CR>", "ab\nxab\n|ab\n"),
    C("/ with \\<", "|a\nxfoo\nfoo\n", "/\\<foo<CR>", "a\nxfoo\n|foo\n"),
    C("/ offset e", "|abc foobar\n", "/foo/e<CR>", "abc fo|obar\n"),
    C("/ offset e-1", "|abc foobar\n", "/foo/e-1<CR>", "abc f|oobar\n"),
    C("/ offset +1 (a line)", "|a\nfoo\nbar\n", "/foo/+1<CR>", "a\nfoo\n|bar\n"),
    C("/ offset s-1", "|abc foobar\n", "/foo/s-1<CR>", "abc| foobar\n"),
    C("/ offset b+2", "|abc foobar\n", "/foo/b+2<CR>", "abc fo|obar\n"),
    C("n keeps the offset", "|a foo x foo y\n", "/foo/e<CR>n", "a foo x fo|o y\n"),
    C("*", "|foo bar foo\n", "*", "foo bar |foo\n"),
    C("* matches whole words only", "|foo foobar foo\n", "*", "foo foobar |foo\n"),
    C("* wraps", "foo bar |foo\n", "*", "|foo bar foo\n"),
    C("* from the middle of the word", "f|oo bar foo\n", "*", "foo bar |foo\n"),
    C("2*", "|foo foo foo\n", "2*", "foo foo |foo\n"),
    C("#", "foo bar |foo\n", "#", "|foo bar foo\n"),
    C("# from the first match wraps", "|foo bar foo\n", "#", "foo bar |foo\n"),
    C("* then n", "|foo x foo y foo\n", "*n", "foo x foo y |foo\n"),
    C("# then n goes on backwards", "foo x foo y |foo\n", "#n", "|foo x foo y foo\n"),
    C("* skips to the word when on a blank", "| foo x foo\n", "*", " foo x |foo\n"),
]

MARKS = [
    C("'a returns to the line", "  |a\nb\nc\n", "majj'a", "  |a\nb\nc\n"),
    C("`a returns to the column", "x|yz\nb\nc\n", "majj`a", "x|yz\nb\nc\n"),
    C("'' returns after G", "|a\nb\nc\n", "G''", "|a\nb\nc\n"),
    C("`` returns after G", "a|bc\nd\n", "G``", "a|bc\nd\n"),
    C("'' swaps with the previous position", "|a\nb\nc\n", "G''''", "a\nb\n|c\n"),
    C("'[ after a yank", "a\n|b\nc\n", "yjG'[", "a\n|b\nc\n"),
    C("`] after yw", "|foo bar\n", "yw`]", "foo| bar\n"),
    C("`. is the last change", "ab\ncd\n|ef\n", "xgg`.", "ab\ncd\n|f\n"),
    C("'a is not set", "|a\nb\n", "j'a", "a\n|b\n"),
    R("d'a", "a\nb\nc\nd\n", "majjd'a", "d\n"),
    C("mark follows a deleted line above", "a\nb\n|c\nd\n", "majjggdd'a", "b\n|c\nd\n"),
    C("`< and `> after visual", "|abc def\n", "wve<Esc>0`>", "abc de|f\n"),
]

H_M_L = [
    C("H", numbered(20, 5), "H", numbered(20, 1), env=(10, 40)),
    C("3H", numbered(20, 5), "3H", numbered(20, 3), env=(10, 40)),
    C("L", numbered(20, 2), "L", numbered(20, 8), env=(10, 40)),
    C("2L", numbered(20, 2), "2L", numbered(20, 7), env=(10, 40)),
    C("M", numbered(20, 1), "M", numbered(20, 4), env=(10, 40)),
    C("H after zt", numbered(20, 1), "15Gzt" + "H", numbered(20, 15), env=(10, 40)),
    C("L after zt", numbered(20, 1), "10GztL", numbered(20, 17), env=(10, 40)),
    C("M on a short file", numbered(3, 1), "M", numbered(3, 2), env=(10, 40)),
]


def run(name, *tables):
    with Qemu(cmdline="text", name=f"t-vi-{name}") as vm:
        sr = ScriptRunner(vm)
        for t in tables:
            sr.run_rows(t)
        sr.finish(name)


@test
def vi_motion_basic():
    """vi motions: characters, lines, columns, G/gg, with counts."""
    run("motion-basic", CHAR_LINE)


@test
def vi_motion_words():
    """vi motions: w b e ge W B E, f t ; ,, paragraphs, sentences, brackets."""
    run("motion-words", WORDS, FIND, PARA_SENT, BRACKETS)


@test
def vi_motion_search():
    """vi motions: searches with offsets, n N * #, marks and the jump back, H M L."""
    run("motion-search", SEARCH, MARKS, H_M_L)
