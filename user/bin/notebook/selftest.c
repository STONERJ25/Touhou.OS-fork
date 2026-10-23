/* notebook --selftest: randomised tests of the document model (gap buffer, line index, undo history), the word wrap rules,
 * the highlighters and the file layer. Runs on the console without a window server. Prints NOTEBOOK SELFTEST PASS or FAIL. */
#include "notebook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t g_rng = 20251023u;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static size_t rndn(size_t n) { return n ? (size_t)(rnd() % (uint32_t)n) : 0; }

/* ---------------------------------------------------------------- reference model */

typedef struct { char *t; size_t len, cap; } Ref;

static void ref_init(Ref *r) { r->cap = 1024; r->t = malloc(r->cap); r->len = 0; }

static void ref_replace(Ref *r, size_t pos, size_t del, const char *ins, size_t n) {
    if (r->len - del + n + 1 > r->cap) { r->cap = (r->len + n) * 2 + 64; r->t = realloc(r->t, r->cap); }
    memmove(r->t + pos + n, r->t + pos + del, r->len - pos - del);
    memcpy(r->t + pos, ins, n);
    r->len = r->len - del + n;
}

static void check_doc(Doc *d, const Ref *r, const char *what) {
    CHECK(doc_len(d) == r->len, "%s: length %zu, expected %zu", what, doc_len(d), r->len);
    if (doc_len(d) != r->len) return;
    char *tmp = malloc(r->len + 1);
    doc_get(d, 0, r->len, tmp);
    CHECK(memcmp(tmp, r->t, r->len) == 0, "%s: text differs", what);
    free(tmp);
    int nl = 1;
    for (size_t i = 0; i < r->len; i++) if (r->t[i] == '\n') nl++;
    CHECK(d->nls == nl, "%s: %d lines, expected %d", what, d->nls, nl);
    if (d->nls != nl) return;
    int k = 1;
    CHECK(d->ls[0] == 0, "%s: first line start", what);
    for (size_t i = 0; i < r->len; i++)
        if (r->t[i] == '\n') {
            CHECK(d->ls[k] == i + 1, "%s: line %d starts at %zu, expected %zu", what, k, d->ls[k], i + 1);
            k++;
        }
}

static size_t random_text(char *out, size_t max) {
    static const char *const pieces[] = { "a", "b", "hello", " ", "\n", "\n\n", "x\ny", "\t", "\xE3\x81\x82", "\xC3\xA9", "  indent", "Reimu", "\xF0\x9F\x8C\xB8" };
    size_t n = 0, parts = 1 + rndn(4);
    for (size_t i = 0; i < parts; i++) {
        const char *p = pieces[rndn(sizeof pieces / sizeof pieces[0])];
        size_t l = strlen(p);
        if (n + l > max) break;
        memcpy(out + n, p, l);
        n += l;
    }
    return n;
}

/* ---------------------------------------------------------------- tests */

static void test_memmove(void) {
    unsigned char *a = malloc(4096), *b = malloc(4096);
    for (int round = 0; round < 400; round++) {
        for (int i = 0; i < 4096; i++) a[i] = b[i] = (unsigned char)rnd();
        size_t n = rndn(1500), s = rndn(2000), d = rndn(2000);
        nb_memmove(a + d, a + s, n);
        memmove(b + d, b + s, n);
        CHECK(memcmp(a, b, 4096) == 0, "memmove n=%zu s=%zu d=%zu", n, s, d);
    }
    free(a); free(b);
}

static int dec(const char *s, size_t n, uint32_t *cp) { return nb_u8_dec(s, s + n, cp); }

static void test_utf8(void) {
    uint32_t cp;
    CHECK(dec("a", 1, &cp) == 1 && cp == 'a', "ascii");
    CHECK(dec("\xC3\xA9", 2, &cp) == 2 && cp == 0xE9, "2 bytes");
    CHECK(dec("\xE3\x81\x82", 3, &cp) == 3 && cp == 0x3042, "3 bytes");
    CHECK(dec("\xF0\x9F\x8C\xB8", 4, &cp) == 4 && cp == 0x1F338, "4 bytes");
    CHECK(dec("\xC3", 1, &cp) == 1 && cp == 0xFFFD, "truncated sequence");
    CHECK(dec("\x80", 1, &cp) == 1 && cp == 0xFFFD, "stray continuation byte");
    CHECK(dec("\xC0\x80", 2, &cp) == 1, "overlong");
    CHECK(dec("\xED\xA0\x80", 3, &cp) == 1, "surrogate");
    CHECK(nb_u8_valid("plain \xE3\x81\x82 text", 14), "valid text");
    CHECK(!nb_u8_valid("bad \xFF byte", 10), "invalid text");
    CHECK(nb_u8_dec("", "", &cp) == 0, "empty");
}

static void test_doc_random(void) {
    Doc d;
    Ref r;
    doc_init(&d);
    ref_init(&r);
    char buf[256];
    for (int step = 0; step < 4000; step++) {
        size_t n = random_text(buf, sizeof buf);
        size_t pos = rndn(r.len + 1);
        size_t del = rndn(4) == 0 ? rndn(r.len - pos + 1 > 40 ? 40 : r.len - pos + 1) : 0;
        if (rndn(3) == 0) n = 0;
        doc_edit(&d, pos, del, buf, n, pos + n, DK_OTHER);
        ref_replace(&r, pos, del, buf, n);
        if (step % 37 == 0 || step < 50) check_doc(&d, &r, "random edit");
        if (step % 211 == 0 && r.len > 8) {                      /* contiguous views across the gap */
            size_t a = rndn(r.len), b = a + rndn(r.len - a + 1);
            const char *p = doc_ptr(&d, a, b);
            CHECK(memcmp(p, r.t + a, b - a) == 0, "doc_ptr [%zu,%zu)", a, b);
        }
        if (step % 500 == 499) { const char *all = doc_all(&d); CHECK(memcmp(all, r.t, r.len) == 0, "doc_all"); }
    }
    check_doc(&d, &r, "after random edits");
    /* line queries */
    for (int i = 0; i < 200; i++) {
        size_t off = rndn(r.len + 1);
        int l = doc_line_of(&d, off);
        CHECK(doc_line_start(&d, l) <= off && (l + 1 >= d.nls || off < doc_line_start(&d, l + 1)), "line_of(%zu)", off);
    }
    doc_free(&d);
    free(r.t);
}

static void test_undo(void) {
    Doc d;
    doc_init(&d);
    char *snap[300];
    size_t snaplen[300];
    int ns = 0;
    snap[ns] = strdup("");
    snaplen[ns++] = 0;
    Ref r;
    ref_init(&r);
    char buf[256];
    int saved_at = -1;
    for (int step = 0; step < 150; step++) {
        size_t n = random_text(buf, sizeof buf);
        size_t pos = rndn(r.len + 1);
        size_t del = rndn(3) == 0 ? rndn(r.len - pos + 1 > 20 ? 20 : r.len - pos + 1) : 0;
        if (!n && !del) n = 1, buf[0] = 'z';
        doc_edit(&d, pos, del, buf, n, pos + n, DK_OTHER);
        ref_replace(&r, pos, del, buf, n);
        snap[ns] = malloc(r.len + 1);
        memcpy(snap[ns], r.t, r.len);
        snaplen[ns++] = r.len;
        if (step == 70) { doc_mark_saved(&d); saved_at = ns - 1; }
    }
    CHECK(doc_modified(&d), "modified after edits past the save point");
    for (int i = ns - 1; i > 0; i--) {                           /* undo everything, checking every state */
        CHECK(doc_undo(&d), "undo %d", i);
        Ref s = { snap[i - 1], snaplen[i - 1], snaplen[i - 1] + 1 };
        check_doc(&d, &s, "undo");
        CHECK(doc_modified(&d) == (i - 1 != saved_at), "modified flag at step %d (saved at %d)", i - 1, saved_at);
    }
    CHECK(!doc_undo(&d), "nothing left to undo");
    for (int i = 1; i < ns; i++) {                               /* redo everything */
        CHECK(doc_redo(&d), "redo %d", i);
        Ref s = { snap[i], snaplen[i], snaplen[i] + 1 };
        check_doc(&d, &s, "redo");
        CHECK(doc_modified(&d) == (i != saved_at), "modified flag after redo %d", i);
    }
    CHECK(!doc_redo(&d), "nothing left to redo");
    for (int i = 0; i < ns; i++) free(snap[i]);
    free(r.t);
    doc_free(&d);
}

static void type_str(Doc *d, const char *s) {
    for (; *s; s++) doc_edit(d, d->caret, 0, s, 1, d->caret + 1, DK_TYPE);
}

static void test_merge(void) {
    Doc d;
    doc_init(&d);
    doc_set_text(&d, "", 0);
    type_str(&d, "hello world");
    CHECK(d.nu == 2, "two undo steps for two words, got %d", d.nu);
    doc_undo(&d);
    Ref r = { (char *)"hello ", 6, 7 };
    check_doc(&d, &r, "undo second word");
    doc_undo(&d);
    r.len = 0;
    check_doc(&d, &r, "undo first word");
    doc_redo(&d); doc_redo(&d);
    r.t = (char *)"hello world"; r.len = 11;
    check_doc(&d, &r, "redo both words");
    /* backspace merging: three backspaces are one step */
    for (int i = 0; i < 3; i++) {
        size_t p = d.caret - 1;
        doc_edit(&d, p, 1, "", 0, p, DK_BACKSPACE);
    }
    r.t = (char *)"hello wo"; r.len = 8;
    check_doc(&d, &r, "after backspaces");
    doc_undo(&d);
    r.t = (char *)"hello world"; r.len = 11;
    check_doc(&d, &r, "one undo restores all three");
    CHECK(d.caret == 11, "caret after undo is %zu", d.caret);
    /* a newline is its own step and a caret move breaks the group */
    doc_break_undo(&d);
    type_str(&d, "\nab");
    CHECK(d.nls == 2, "lines after newline");
    doc_group_begin(&d);
    doc_edit(&d, 0, 0, "A", 1, 1, DK_OTHER);
    doc_edit(&d, 1, 0, "B", 1, 2, DK_OTHER);
    doc_group_end(&d);
    doc_undo(&d);
    r.t = (char *)"hello world\nab"; r.len = 14;
    check_doc(&d, &r, "group undone together");
    doc_free(&d);
}

static void test_find(void) {
    Doc d;
    doc_init(&d);
    const char *text = "One Reimu, two reimu, three REIMU\nand Marisa";
    doc_set_text(&d, text, strlen(text));
    CHECK(nb_find(&d, "reimu", 5, 0, true, false) == 15, "case sensitive forward");
    CHECK(nb_find(&d, "reimu", 5, 0, false, false) == 4, "case insensitive forward");
    CHECK(nb_find(&d, "reimu", 5, 5, false, false) == 15, "next after first");
    CHECK(nb_find(&d, "reimu", 5, 100, false, true) == 28, "backward from the end");
    CHECK(nb_find(&d, "reimu", 5, 27, false, true) == 15, "backward before");
    CHECK(nb_find(&d, "Marisa", 6, 0, true, false) == 38, "across a newline");
    CHECK(nb_find(&d, "nothing", 7, 0, true, false) == SIZE_MAX, "no match");
    CHECK(nb_count_matches(&d, "reimu", 5, false, 100) == 3, "count");
    CHECK(nb_count_matches(&d, "reimu", 5, true, 100) == 1, "count case");
    doc_free(&d);
}

static void test_wrap(void) {
    static const char *const lines[] = {
        "the quick brown fox jumps over the lazy dog and keeps running through the bamboo forest",
        "averyveryverylongwordwithoutanyspacesthatmustbebrokenhardsomewhereinthemiddle",
        "\tindented\ttabs\tand\tmore\ttabs\tthat\twrap\talso",
        "",
        "x",
    };
    for (unsigned li = 0; li < sizeof lines / sizeof lines[0]; li++) {
        for (int cols = 4; cols <= 40; cols += 3) {
            const char *t = lines[li];
            size_t n = strlen(t);
            int rows = ed_wrap_count(t, 0, n, cols, 4, NULL, 0);
            size_t *rs = malloc((size_t)rows * sizeof(size_t));
            int rows2 = ed_wrap_count(t, 0, n, cols, 4, rs, rows);
            CHECK(rows == rows2 && rows >= 1, "row count stable");
            CHECK(rs[0] == 0, "first row starts at 0");
            for (int r = 1; r < rows; r++) CHECK(rs[r] > rs[r - 1] && rs[r] < n, "row starts increase (line %u cols %d row %d)", li, cols, r);
            for (int r = 0; r < rows; r++) {                    /* no row is wider than cols unless it is one unbreakable cell */
                size_t a = rs[r], b = r + 1 < rows ? rs[r + 1] : n;
                int col = 0;
                for (size_t i = a; i < b; i++) col += t[i] == '\t' ? 4 - col % 4 : 1;
                CHECK(col <= cols + 1 || b - a == 1, "row %d too wide: %d > %d (line %u)", r, col, cols, li);
            }
            free(rs);
        }
    }
}

static void test_syntax(void) {
    uint8_t cls[64];
    memset(cls, 0xEE, sizeof cls);
    syn_scan(LANG_C, "x = 1;", 6, 0, cls);
    CHECK(cls[0] == SC_NORMAL && cls[1] == SC_NORMAL && cls[2] == SC_NORMAL && cls[4] == SC_NUMBER, "unrecognised text is plain, whatever the buffer held before");
    SynState st = syn_scan(LANG_C, "int x = 5; /* open", 18, 0, cls);
    CHECK(st == 1, "block comment opens");
    CHECK(cls[0] == SC_TYPE && cls[1] == SC_TYPE && cls[2] == SC_TYPE, "int is a type");
    CHECK(cls[8] == SC_NUMBER, "number");
    CHECK(cls[11] == SC_COMMENT, "comment text");
    st = syn_scan(LANG_C, "still */ return 1; // x", 23, 1, cls);
    CHECK(st == 0, "block comment closes");
    CHECK(cls[2] == SC_COMMENT && cls[9] == SC_KEYWORD && cls[20] == SC_COMMENT, "mixed line");
    st = syn_scan(LANG_C, "#include <stdio.h>", 18, 0, cls);
    CHECK(cls[0] == SC_PREPROC && cls[9] == SC_STRING, "preprocessor line");
    st = syn_scan(LANG_C, "char *s = \"a\\\"b\";", 17, 0, cls);
    CHECK(cls[10] == SC_STRING && cls[14] == SC_STRING, "string with an escaped quote");
    st = syn_scan(LANG_SHELL, "echo \"$HOME\" # note", 19, 0, cls);
    CHECK(cls[0] == SC_PREPROC && cls[5] == SC_STRING && cls[15] == SC_COMMENT, "shell line");
    st = syn_scan(LANG_MARKDOWN, "```c", 4, 0, cls);
    CHECK(st == 1, "markdown fence opens");
    st = syn_scan(LANG_MARKDOWN, "int x;", 6, 1, cls);
    CHECK(st == 1 && cls[0] == SC_CODE, "inside the fence");
    st = syn_scan(LANG_MARKDOWN, "```", 3, 1, cls);
    CHECK(st == 0, "markdown fence closes");
    syn_scan(LANG_MARKDOWN, "# Title", 7, 0, cls);
    CHECK(cls[0] == SC_HEADING && cls[6] == SC_HEADING, "heading");
    syn_scan(LANG_MARKDOWN, "- item with `code` and **bold**", 30, 0, cls);
    CHECK(cls[0] == SC_MARKER && cls[13] == SC_CODE && cls[24] == SC_EMPHASIS, "list, code, emphasis");
    CHECK(syn_detect("/x/main.c", NULL, 0) == LANG_C, "detect C");
    CHECK(syn_detect("/x/run.sh", NULL, 0) == LANG_SHELL, "detect shell");
    CHECK(syn_detect("/x/README.MD", NULL, 0) == LANG_MARKDOWN, "detect markdown");
    CHECK(syn_detect("/x/script", "#!/bin/sh", 9) == LANG_SHELL, "detect shebang");
    CHECK(syn_detect("/x/notes.txt", NULL, 0) == LANG_PLAIN, "plain");
}

static void test_fileio(void) {
    FileInfo fi;
    char a[] = "one\r\ntwo\r\nthree";
    size_t n = nb_normalize(a, sizeof a - 1, &fi);
    CHECK(n == 13 && fi.eol == EOL_CRLF && !fi.mixed && !memcmp(a, "one\ntwo\nthree", 13), "CRLF text");
    char b[] = "\xEF\xBB\xBF" "x\ny\n";
    n = nb_normalize(b, sizeof b - 1, &fi);
    CHECK(fi.bom && n == 4 && !memcmp(b, "x\ny\n", 4) && fi.eol == EOL_LF, "BOM");
    char c[] = "a\rb\rc\r";
    n = nb_normalize(c, sizeof c - 1, &fi);
    CHECK(fi.eol == EOL_CR && n == 6 && !memcmp(c, "a\nb\nc\n", 6), "classic Mac line endings");
    char m[] = "a\r\nb\nc\r\nd";
    n = nb_normalize(m, sizeof m - 1, &fi);
    CHECK(fi.eol == EOL_CRLF && fi.mixed && n == 7, "mixed endings");
    char bad[] = "caf\xE9 au lait";
    nb_normalize(bad, sizeof bad - 1, &fi);
    CHECK(!fi.utf8_ok, "invalid UTF-8 is noticed");
    CHECK(nb_find_nul("ab\0cd", 5) == 2 && nb_find_nul("abcd", 4) == -1, "NUL detection");

    /* write with the three line ending styles and read back */
    Doc d;
    doc_init(&d);
    doc_set_text(&d, "alpha\nbeta\n\ngamma", 17);
    static const char *const expect[] = { "alpha\nbeta\n\ngamma", "alpha\r\nbeta\r\n\r\ngamma", "alpha\rbeta\r\rgamma" };
    for (int st = 0; st < 3; st++) {
        const char *path = "/tmp/nb-selftest.txt";
        int rc = nb_write_doc(path, &d, (EolStyle)st, st == 1);
        CHECK(rc == 0, "write failed: %d", rc);
        char *data = NULL;
        size_t len = 0;
        rc = nb_read_all(path, &data, &len);
        CHECK(rc == 0, "read back failed: %d", rc);
        if (!rc) {
            const char *want = expect[st];
            size_t off = st == 1 ? 3 : 0;
            CHECK(len == strlen(want) + off && !memcmp(data + off, want, strlen(want)), "style %d wrote %zu bytes", st, len);
            if (st == 1) CHECK(!memcmp(data, "\xEF\xBB\xBF", 3), "BOM written");
            free(data);
        }
        unlink(path);
    }
    doc_free(&d);
    char out[128];
    nb_abspath("/a/b/../c//d/./e", out, sizeof out);
    CHECK(!strcmp(out, "/a/c/d/e"), "abspath: %s", out);
    nb_abspath("/..", out, sizeof out);
    CHECK(!strcmp(out, "/"), "abspath root: %s", out);
}

/* ---------------------------------------------------------------- the editor operations */

static void verify_doc(Doc *d, const char *what) {
    size_t len = doc_len(d);
    const char *t = doc_all(d);
    int nl = 1;
    for (size_t i = 0; i < len; i++) if (t[i] == '\n') nl++;
    CHECK(d->nls == nl, "%s: %d lines in the index, %d in the text", what, d->nls, nl);
    if (d->nls == nl) {
        int k = 1;
        for (size_t i = 0; i < len; i++) if (t[i] == '\n') { CHECK(d->ls[k] == i + 1, "%s: line %d start %zu != %zu", what, k, d->ls[k], i + 1); k++; }
    }
    CHECK(d->caret <= len && d->anchor <= len, "%s: caret %zu anchor %zu beyond %zu", what, d->caret, d->anchor, len);
}

static void key(Editor *e, int code, int mods) {
    KKey k = { (uint8_t)code, (uint8_t)mods, 1, 0 };
    ed_key(e, NULL, &k);
}

static void typestr(Editor *e, const char *s) {
    for (; *s; s++) {
        if (*s == '\n') key(e, KEY_ENTER, 0);
        else { KKey k = { KEY_Q, 0, 1, (uint32_t)(unsigned char)*s }; ed_key(e, NULL, &k); }
    }
}

static char *text_of(Editor *e) {
    size_t n = doc_len(&e->doc);
    char *s = malloc(n + 1);
    doc_get(&e->doc, 0, n, s);
    s[n] = 0;
    return s;
}

static bool is_text(Editor *e, const char *want) {
    char *s = text_of(e);
    bool ok = !strcmp(s, want);
    if (!ok) printf("  got [%s] wanted [%s]\n", s, want);
    free(s);
    return ok;
}

static void test_editor_ops(void) {
    Editor *e = &A.ed;
    ed_init(e);
    A.readonly = false;
    doc_set_text(&e->doc, "", 0);
    e->spaces = true;
    e->tab_w = 4;

    typestr(e, "  foo\nbar");
    CHECK(is_text(e, "  foo\n  bar"), "auto-indent keeps the indentation");
    key(e, KEY_TAB, 0);
    CHECK(is_text(e, "  foo\n  bar   "), "Tab goes to the next tab stop (column 8)");
    key(e, KEY_TAB, MOD_SHIFT);
    CHECK(is_text(e, "  foo\nbar   "), "Shift+Tab removes the indentation of the line");

    /* indent and unindent a selection of three lines */
    doc_set_text(&e->doc, "one\n\ntwo\nthree\nfour", 19);
    ed_set_selection(e, 0, 14);                                  /* from the start of "one" into "three" */
    ed_indent(e, false);
    CHECK(is_text(e, "    one\n\n    two\n    three\nfour"), "indent skips the blank line");
    CHECK(e->doc.anchor == 0 && e->doc.caret > 14, "the selection still covers the lines");
    ed_indent(e, true);
    CHECK(is_text(e, "one\n\ntwo\nthree\nfour"), "unindent restores the text");
    ed_undo(e);
    CHECK(is_text(e, "    one\n\n    two\n    three\nfour"), "undo of unindent is one step");
    ed_undo(e);
    CHECK(is_text(e, "one\n\ntwo\nthree\nfour"), "undo of indent is one step");
    e->spaces = false;
    ed_set_selection(e, 0, 3);
    ed_indent(e, false);
    CHECK(is_text(e, "\tone\n\ntwo\nthree\nfour"), "indent with real tabs");
    e->spaces = true;

    /* line operations */
    doc_set_text(&e->doc, "a\nb\nc", 5);
    ed_set_selection(e, 2, 2);                                   /* in "b" */
    ed_duplicate_line(e);
    CHECK(is_text(e, "a\nb\nb\nc"), "duplicate line");
    ed_move_lines(e, -1);
    CHECK(is_text(e, "a\nb\nb\nc"), "moving equal lines up");
    ed_set_selection(e, 4, 4);                                   /* second "b" */
    ed_delete_line(e);
    CHECK(is_text(e, "a\nb\nc"), "delete line");
    ed_set_selection(e, 0, 0);
    ed_move_lines(e, 1);
    CHECK(is_text(e, "b\na\nc"), "move line down");
    ed_move_lines(e, 1);
    CHECK(is_text(e, "b\nc\na"), "move line down again");
    ed_move_lines(e, 1);
    CHECK(is_text(e, "b\nc\na"), "the last line cannot move down");
    ed_set_selection(e, 4, 4);                                   /* in "a", the last line */
    ed_delete_line(e);
    CHECK(is_text(e, "b\nc"), "delete the last line removes the line break before it");

    /* braces in C */
    e->lang = LANG_C;
    doc_set_text(&e->doc, "int f() {}", 10);
    ed_set_selection(e, 9, 9);
    key(e, KEY_ENTER, 0);
    CHECK(is_text(e, "int f() {\n    \n}"), "{|} opens a block");
    CHECK(e->doc.caret == 14, "caret on the indented line (%zu)", e->doc.caret);
    e->lang = LANG_PLAIN;

    /* word wrap and vertical movement */
    char longline[256];
    memset(longline, 'x', sizeof longline - 1);
    for (int i = 9; i < 255; i += 10) longline[i] = ' ';
    longline[255] = 0;
    doc_set_text(&e->doc, longline, 255);
    e->wrap = true;
    e->cols = 40;
    e->vis_rows = 5;
    ed_invalidate(e);
    int rows = ed_rows_in_line(e, 0);
    CHECK(rows >= 6 && rows <= 8, "255 characters in 40 columns need about 7 rows, got %d", rows);
    ed_set_selection(e, 0, 0);
    key(e, KEY_DOWN, 0);
    CHECK(e->doc.caret >= 30 && e->doc.caret <= 44, "Down moves one visual row (caret %zu)", e->doc.caret);
    size_t first_row = e->doc.caret;
    key(e, KEY_END, 0);
    CHECK(e->doc.caret > first_row && e->doc.caret < 90, "End goes to the end of the row (%zu)", e->doc.caret);
    key(e, KEY_UP, 0);
    CHECK(e->doc.caret < 40, "Up moves back to the first row (%zu)", e->doc.caret);
    for (int i = 0; i < 20; i++) key(e, KEY_DOWN, 0);
    CHECK(e->doc.caret == 255, "Down on the last row goes to the end (%zu)", e->doc.caret);
    e->wrap = false;
    ed_free(e);
}

static void test_editor_fuzz(void) {
    Editor *e = &A.ed;
    static const int codes[] = { KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_BACKSPACE, KEY_DELETE, KEY_ENTER, KEY_TAB };
    for (int round = 0; round < 6; round++) {
        ed_init(e);
        A.readonly = false;
        const char *start = "The quick brown fox\n\tjumps over\n  the lazy dog;\n\nlast {line}";
        doc_set_text(&e->doc, start, strlen(start));
        e->wrap = round & 1;
        e->spaces = (round & 2) == 0;
        e->tab_w = round % 3 == 0 ? 2 : round % 3 == 1 ? 4 : 8;
        e->cols = 12 + round * 7;
        e->vis_rows = 3 + round;
        e->lang = round % 2 ? LANG_C : LANG_PLAIN;
        for (int step = 0; step < 1500; step++) {
            int r = (int)rndn(100);
            int mods = rndn(4) == 0 ? MOD_SHIFT : 0;
            if (r < 40) {
                static const char chars[] = "abcdef xyz{}();\n";
                char c = chars[rndn(sizeof chars - 1)];
                if (c == '\n') key(e, KEY_ENTER, 0);
                else { KKey k = { KEY_Q, 0, 1, (uint32_t)(unsigned char)c }; ed_key(e, NULL, &k); }
            } else if (r < 80) key(e, codes[rndn(sizeof codes / sizeof codes[0])], mods);
            else if (r < 84) key(e, KEY_Z, MOD_CTRL);
            else if (r < 88) key(e, KEY_Y, MOD_CTRL);
            else if (r < 90) key(e, KEY_D, MOD_CTRL | MOD_SHIFT);
            else if (r < 92) key(e, KEY_K, MOD_CTRL | MOD_SHIFT);
            else if (r < 94) key(e, rndn(2) ? KEY_UP : KEY_DOWN, MOD_ALT);
            else if (r < 96) key(e, rndn(2) ? KEY_LEFT : KEY_RIGHT, MOD_CTRL | mods);
            else if (r < 97) key(e, rndn(2) ? KEY_BACKSPACE : KEY_DELETE, MOD_CTRL);
            else if (r < 98) key(e, KEY_A, MOD_CTRL);
            else if (r < 99) ed_goto(e, (int)rndn(8), (int)rndn(10));
            else ed_indent(e, rndn(2));
            if (step % 25 == 0) verify_doc(&e->doc, "fuzz");
        }
        verify_doc(&e->doc, "fuzz end");
        char *final_text = text_of(e);
        int guard = 0;
        while (doc_undo(&e->doc) && guard++ < 100000) { }
        CHECK(is_text(e, start), "undoing the whole fuzz history restores the start (round %d)", round);
        CHECK(!doc_modified(&e->doc), "back at the start, the page is not modified");
        guard = 0;
        while (doc_redo(&e->doc) && guard++ < 100000) { }
        CHECK(is_text(e, final_text), "redoing everything reproduces the final text (round %d)", round);
        free(final_text);
        verify_doc(&e->doc, "after redo");
        ed_free(e);
    }
}

static void test_big_and_bench(void) {
    /* 100,000 lines: build, index, random access and edits stay fast */
    size_t lines = 100000;
    char *text = malloc(lines * 8 + 16);
    size_t n = 0;
    for (size_t i = 1; i <= lines; i++) n += (size_t)sprintf(text + n, "%zu\n", i);
    Doc d;
    doc_init(&d);
    uint64_t t0 = k_now_ms();
    CHECK(doc_set_text(&d, text, n), "set_text");
    uint64_t t1 = k_now_ms();
    CHECK(d.nls == (int)lines + 1, "line count %d", d.nls);
    for (int i = 0; i < 2000; i++) {
        size_t pos = rndn(doc_len(&d));
        doc_edit(&d, pos, 0, "ab\n", 3, pos + 3, DK_OTHER);
    }
    uint64_t t2 = k_now_ms();
    CHECK(d.nls == (int)lines + 1 + 2000, "line count after edits %d", d.nls);
    while (doc_undo(&d)) { }
    uint64_t t3 = k_now_ms();
    CHECK(d.nls == (int)lines + 1 && doc_len(&d) == n, "undone");
    printf("bench: load %llu ms, 2000 edits %llu ms, undo all %llu ms\n", (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1), (unsigned long long)(t3 - t2));
    doc_free(&d);
    free(text);
    size_t mb = 4u << 20;
    char *x = malloc(mb), *y = malloc(mb);
    memset(x, 7, mb);
    t0 = k_now_ms();
    memcpy(y, x, mb);
    t1 = k_now_ms();
    nb_memmove(y, x, mb);
    t2 = k_now_ms();
    printf("bench: 4 MB libc memcpy %llu ms, nb_memmove %llu ms\n", (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));
    free(x); free(y);
}

int selftest_run(void) {
    test_memmove();
    test_utf8();
    test_doc_random();
    test_undo();
    test_merge();
    test_find();
    test_wrap();
    test_syntax();
    test_fileio();
    test_editor_ops();
    test_editor_fuzz();
    test_big_and_bench();
    if (g_fail) printf("NOTEBOOK SELFTEST FAIL (%d of %d checks)\n", g_fail, g_checks);
    else printf("NOTEBOOK SELFTEST PASS (%d checks)\n", g_checks);
    return g_fail ? 1 : 0;
}
