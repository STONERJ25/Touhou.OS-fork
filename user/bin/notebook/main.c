/* Patchouli's Notebook: the text editor of the Gensokyo desktop. This file is the application proper: the window, menus,
 * status bar, shortcuts and the file flows (open, save, save as, close, quit with the "unsaved changes" question).
 *
 *   notebook [file]         open a file (a path that does not exist yet starts an empty page with that name)
 *   notebook --selftest     run the document model tests on the console (no window server needed)
 *
 * NOTEBOOK_TRACE=1 in the environment makes the editor print "nb: ..." progress lines to stdout (used by the tests). */
#include "notebook.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

App A;

/* ---------------------------------------------------------------- messages and tracing */

void nb_trace(const char *fmt, ...) {
    if (!A.trace) return;
    char buf[400];
    int n = snprintf(buf, sizeof buf, "nb: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (write(1, buf, (size_t)n) < 0) { }
}

void msg_set(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.msg, sizeof A.msg, fmt, ap);
    va_end(ap);
    A.msg_until = k_now_ms() + 5000;
    if (!A.in_draw && A.app) kapp_redraw(A.app);
    nb_trace("msg %s", A.msg);
}

/* ---------------------------------------------------------------- the document as a file */

static bool is_modified(void) { return doc_modified(&A.ed.doc); }

static void set_name(const char *path) {
    if (path && *path) snprintf(A.name, sizeof A.name, "%s", k_basename(path));
    else snprintf(A.name, sizeof A.name, "untitled");
}

static void reset_empty(void) {
    doc_set_text(&A.ed.doc, "", 0);
    A.path[0] = 0;
    A.has_path = false;
    A.eol = EOL_LF;
    A.eol_mixed = A.bom = false;
    A.utf8_ok = true;
    A.readonly = false;
    A.mtime = 0;
    A.ed.lang = LANG_PLAIN;
    A.ed.spaces = A.pref_spaces;
    ed_reset_view(&A.ed);
    A.count_valid = false;
    set_name(NULL);
}

static size_t saved_size(void) {
    size_t len = doc_len(&A.ed.doc);
    if (A.eol == EOL_CRLF) len += (size_t)doc_nlines(&A.ed.doc) - 1;
    if (A.bom) len += 3;
    return len;
}

static void fmt_size(char *out, size_t cap, size_t n) {
    if (n < 1024) snprintf(out, cap, "%zu byte%s", n, n == 1 ? "" : "s");
    else if (n < 1024 * 1024) snprintf(out, cap, "%zu.%zu KB", n / 1024, n * 10 / 1024 % 10);
    else snprintf(out, cap, "%zu.%zu MB", n >> 20, (n * 10 >> 20) % 10);
}

/* Guess whether the file indents with tabs (-1: no idea, 0 tabs, 1 spaces). */
static int detect_indent(void) {
    Doc *d = &A.ed.doc;
    int nl = doc_nlines(d) < 400 ? doc_nlines(d) : 400, tabs = 0, spaces = 0;
    for (int l = 0; l < nl; l++) {
        size_t s = doc_line_start(d, l), e = doc_line_end(d, l);
        if (s >= e) continue;
        unsigned char c = doc_byte(d, s);
        if (c == '\t') tabs++;
        else if (c == ' ' && e - s >= 3 && doc_byte(d, s + 1) == ' ') spaces++;
    }
    if (tabs == 0 && spaces == 0) return -1;
    return tabs > spaces ? 0 : 1;
}

static void error_box(const char *title, const char *what, const char *path, int err) {
    char msg[NB_PATH_MAX + 200];
    if (err) snprintf(msg, sizeof msg, "%s\n%s\n\n%s", what, path, strerror(err));
    else snprintf(msg, sizeof msg, "%s\n%s", what, path);
    nb_trace("error %s: %s", what, path);
    dlg_message(A.app, title, msg, KI_ERROR);
}

/* Loads a file into the editor. Returns true when the page was replaced (the old page is kept when something goes wrong). */
static bool load_path(const char *path_in, bool must_exist) {
    char path[NB_PATH_MAX];
    nb_abspath(path_in, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) {
        int err = errno;
        if (err == ENOENT && !must_exist) {                     /* a new file: an empty page that will be created by Save */
            reset_empty();
            snprintf(A.path, sizeof A.path, "%s", path);
            A.has_path = true;
            set_name(path);
            A.ed.lang = syn_detect(path, NULL, 0);
            msg_set("New file: %s does not exist yet. Save creates it.", A.name);
            nb_trace("open %s new", path);
            return true;
        }
        if (err == ENOENT) recent_remove(path);
        error_box("Cannot open file", err == ENOENT ? "This file does not exist (any more):" : "Cannot open:", path, err == ENOENT ? 0 : err);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        error_box("Cannot open folder", "This is a folder, not a text file. Use the Explorer to browse folders:", path, 0);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error_box("Cannot open file", "This is not a regular file (a device, pipe or socket):", path, 0);
        return false;
    }
    char *data = NULL;
    size_t len = 0;
    int rc = nb_read_all(path, &data, &len);
    if (rc) {
        error_box(rc == EFBIG ? "File too large" : "Cannot open file", rc == EFBIG ? "This file is larger than 64 MB, too big for the Notebook:" : "Cannot read:", path, rc == EFBIG ? 0 : rc);
        return false;
    }
    long nul = nb_find_nul(data, len);
    if (nul >= 0) {
        bool utf16 = len >= 2 && (((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) || ((unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF));
        free(data);
        nb_trace("refused binary %s", path);
        char msg[NB_PATH_MAX + 260];
        if (utf16) snprintf(msg, sizeof msg, "%s\n\nThis file is UTF-16 text. Notebook only edits UTF-8 text, so it was not opened.", path);
        else snprintf(msg, sizeof msg, "%s\n\nThis looks like a binary file (it has a zero byte at position %ld). Notebook only edits text, so it was not opened.", path, nul);
        dlg_message(A.app, "Not a text file", msg, KI_WARNING);
        return false;
    }
    FileInfo fi;
    size_t n = nb_normalize(data, len, &fi);
    if (!doc_set_text(&A.ed.doc, data, n)) {
        free(data);
        error_box("Cannot open file", "Out of memory while reading:", path, 0);
        return false;
    }
    char first[160];
    size_t fl = n < sizeof first - 1 ? n : sizeof first - 1;
    memcpy(first, data, fl);
    free(data);
    char *nl = memchr(first, '\n', fl);
    if (nl) fl = (size_t)(nl - first);
    snprintf(A.path, sizeof A.path, "%s", path);
    A.has_path = true;
    A.eol = fi.eol;
    A.eol_mixed = fi.mixed;
    A.bom = fi.bom;
    A.utf8_ok = fi.utf8_ok;
    A.readonly = access(path, W_OK) != 0;
    A.mtime = st.st_mtime;
    set_name(path);
    A.ed.lang = syn_detect(path, first, fl);
    int ind = detect_indent();
    A.ed.spaces = ind < 0 ? A.pref_spaces : ind == 1;
    ed_reset_view(&A.ed);
    A.count_valid = false;
    recent_add(path);
    if (A.readonly) msg_set("Read-only file. Save As makes an editable copy.");
    else if (fi.mixed) msg_set("Mixed line endings were normalised to %s.", nb_eol_name(fi.eol));
    else if (!fi.utf8_ok) msg_set("Invalid UTF-8 bytes are shown as boxes and kept.");
    nb_trace("open %s lines=%d bytes=%zu eol=%s lang=%s%s", path, doc_nlines(&A.ed.doc), n, nb_eol_name(fi.eol), syn_name(A.ed.lang),
             A.readonly ? " readonly" : "");
    return true;
}

static bool write_to(const char *path, bool new_identity) {
    int rc = nb_write_doc(path, &A.ed.doc, A.eol, A.bom);
    if (rc) {
        error_box("Cannot save", "The page could not be written to:", path, rc);
        return false;
    }
    if (new_identity) {
        snprintf(A.path, sizeof A.path, "%s", path);
        A.has_path = true;
        set_name(path);
        A.ed.lang = syn_detect(path, NULL, 0);
        if (A.ed.lang == LANG_PLAIN) {
            char first[100];
            size_t n = doc_line_end(&A.ed.doc, 0);
            if (n > sizeof first - 1) n = sizeof first - 1;
            doc_get(&A.ed.doc, 0, n, first);
            A.ed.lang = syn_detect(path, first, n);
        }
        A.readonly = false;
        ed_invalidate(&A.ed);
    }
    doc_mark_saved(&A.ed.doc);
    struct stat st;
    A.mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
    recent_add(path);
    char sz[32];
    fmt_size(sz, sizeof sz, saved_size());
    msg_set("Saved %s (%s).", A.name, sz);
    nb_trace("saved %s bytes=%zu", path, saved_size());
    return true;
}

/* ---------------------------------------------------------------- what to do after the unsaved-changes question */

typedef enum { ACT_NONE, ACT_QUIT, ACT_CLOSE, ACT_NEW, ACT_OPEN } ActKind;
static struct { ActKind kind; char path[NB_PATH_MAX]; bool valid; } g_pend;

static void perform(ActKind k, const char *path) {
    switch (k) {
    case ACT_QUIT:
        A.quitting = true;
        nb_trace("quit");
        kapp_quit(A.app, 0);
        break;
    case ACT_CLOSE:
    case ACT_NEW:
        reset_empty();
        msg_set(k == ACT_NEW ? "A fresh page." : "Page closed.");
        break;
    case ACT_OPEN:
        if (path) load_path(path, true);
        break;
    default:
        break;
    }
}

static void begin_save_as(void);
static void save_flow(void);

/* Called after a save that was part of "save, then carry on". */
static void pending_done(void) {
    if (!g_pend.valid) return;
    g_pend.valid = false;
    perform(g_pend.kind, g_pend.path);
}

static void on_unsaved(int choice, void *user) {
    (void)user;
    if (!g_pend.valid) return;
    if (choice == 0) save_flow();                               /* Save, then carry on (g_pend stays valid until the save is done) */
    else if (choice == 1) pending_done();                       /* Don't save: just carry on */
    else g_pend.valid = false;                                  /* Cancel */
}

/* Runs the action once unsaved changes are dealt with. */
static void guard(ActKind k, const char *path) {
    if (!is_modified()) { perform(k, path); return; }
    g_pend.kind = k;
    snprintf(g_pend.path, sizeof g_pend.path, "%s", path ? path : "");
    g_pend.valid = true;
    static const char *const verb[] = { "", "quitting", "closing it", "starting a new page", "opening another file" };
    char msg[400];
    snprintf(msg, sizeof msg, "\"%s\" has changes that were not saved. Save them before %s?", A.name, verb[k]);
    static const char *const labels[] = { "Save", "Don't save", "Cancel" };
    dlg_choice(A.app, "Unsaved changes", msg, KI_QUESTION, labels, 3, 2, on_unsaved, NULL);
}

/* ---------------------------------------------------------------- save and save as */

static char g_ow_path[NB_PATH_MAX];

static void finish_save_as(const char *full) {
    if (write_to(full, true)) pending_done();
    else g_pend.valid = false;
}

static void on_overwrite(bool yes, void *user) {
    (void)user;
    nb_modal_leave();
    if (yes) finish_save_as(g_ow_path); else g_pend.valid = false;
}

static void on_saveas_path(const char *path, void *user) {
    (void)user;
    nb_modal_leave();
    if (!path) { g_pend.valid = false; return; }
    char full[NB_PATH_MAX];
    nb_abspath(path, full, sizeof full);
    struct stat st;
    if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) { error_box("Cannot save", "That is a folder, not a file name:", full, 0); g_pend.valid = false; return; }
        if (!A.has_path || strcmp(full, A.path)) {
            snprintf(g_ow_path, sizeof g_ow_path, "%s", full);
            char msg[NB_PATH_MAX + 120];
            snprintf(msg, sizeof msg, "%s already exists. Do you want to replace it?", full);
            nb_modal_enter();
            nb_trace("dialog replace");
            k_confirm_dialog(A.app, "Replace file?", msg, KI_WARNING, "Replace", "Cancel", on_overwrite, NULL);
            return;
        }
    }
    finish_save_as(full);
}

static void begin_save_as(void) {
    char dir[NB_PATH_MAX];
    const char *defname = "untitled.txt";
    if (A.has_path) {
        snprintf(dir, sizeof dir, "%s", A.path);
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) *sl = 0; else if (sl) sl[1] = 0;
        defname = A.name;
    } else {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof dir, "%s", home && *home ? home : "/");
    }
    nb_modal_enter();
    k_file_dialog(A.app, 1, "Save As", dir, defname, on_saveas_path, NULL);
    nb_trace("dialog save-as");
}

static void on_changed_on_disk(bool yes, void *user) {
    (void)user;
    nb_modal_leave();
    if (yes && write_to(A.path, false)) pending_done();
    else g_pend.valid = false;
}

/* Saves the page (asking where to when it has no file, and before overwriting something that changed on disk). A pending
 * action (g_pend) is carried out once the page is safe. */
static void save_flow(void) {
    if (!A.has_path) { begin_save_as(); return; }
    if (A.readonly) { msg_set("This file is read-only. Choose a new name to keep your changes."); begin_save_as(); return; }
    struct stat st;
    if (A.trace && stat(A.path, &st) == 0) nb_trace("mtime disk=%lld loaded=%lld", (long long)st.st_mtime, (long long)A.mtime);
    if (A.mtime && stat(A.path, &st) == 0 && st.st_mtime != A.mtime) {
        char msg[NB_PATH_MAX + 160];
        snprintf(msg, sizeof msg, "%s was changed by something else after you opened it. Save anyway and overwrite those changes?", A.name);
        nb_modal_enter();
        nb_trace("dialog changed-on-disk");
        k_confirm_dialog(A.app, "File changed on disk", msg, KI_WARNING, "Overwrite", "Cancel", on_changed_on_disk, NULL);
        return;
    }
    if (write_to(A.path, false)) pending_done();
    else g_pend.valid = false;
}

static void act_save(void) { g_pend.valid = false; save_flow(); }
static void act_save_as(void) { g_pend.valid = false; begin_save_as(); }

/* ---------------------------------------------------------------- the other file actions */

static void on_open_path(const char *path, void *user) {
    (void)user;
    nb_modal_leave();
    if (path) guard(ACT_OPEN, path);
}

static void act_open(void) {
    char dir[NB_PATH_MAX];
    if (A.has_path) {
        snprintf(dir, sizeof dir, "%s", A.path);
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) *sl = 0; else if (sl) sl[1] = 0;
    } else {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof dir, "%s", home && *home ? home : "/");
    }
    nb_modal_enter();
    k_file_dialog(A.app, 0, "Open file", dir, "", on_open_path, NULL);
    nb_trace("dialog open");
}

static void act_new(void) { guard(ACT_NEW, NULL); }
static void act_close(void) { guard(ACT_CLOSE, NULL); }
static void act_quit(void) { guard(ACT_QUIT, NULL); }

static void spawn_window(const char *path) {
    pid_t pid = fork();
    if (pid < 0) { msg_set("Could not open another window."); return; }
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        char *argv[3] = { "notebook", (char *)path, NULL };
        execv("/bin/notebook", argv);
        _exit(127);
    }
    A.children++;
}

static void reap_children(void) {
    while (A.children > 0 && waitpid(-1, NULL, WNOHANG) > 0) A.children--;
}

static void on_goto(const char *text, void *user) {
    (void)user;
    nb_modal_leave();
    if (!text) return;
    char *end;
    long line = strtol(text, &end, 10);
    long col = 0;
    while (*end == ' ' || *end == ':' || *end == ',') end++;
    if (*end >= '0' && *end <= '9') col = strtol(end, NULL, 10);
    if (line < 1) { msg_set("Type a line number, for example 120 or 120:5."); return; }
    int nl = doc_nlines(&A.ed.doc);
    if (line > nl) { msg_set("The page only has %d lines; going to the last one.", nl); line = nl; }
    ed_goto(&A.ed, (int)line, (int)col);
    msg_set("Line %ld", line);
}

static void act_goto(void) {
    char prompt[96], initial[24];
    int line, col;
    ed_caret_pos(&A.ed, &line, &col);
    snprintf(prompt, sizeof prompt, "Line number (1-%d) or line:column:", doc_nlines(&A.ed.doc));
    snprintf(initial, sizeof initial, "%s", "");
    (void)line;
    nb_modal_enter();
    k_input_dialog(A.app, "Go to line", prompt, initial, on_goto, NULL);
    nb_trace("dialog goto");
}

/* ---------------------------------------------------------------- menus */

enum {
    MA_NONE, MA_NEW, MA_NEWWIN, MA_OPEN, MA_SAVE, MA_SAVEAS, MA_CLOSE, MA_QUIT, MA_CLEAR_RECENT,
    MA_UNDO, MA_REDO, MA_CUT, MA_COPY, MA_PASTE, MA_SELALL, MA_FIND, MA_FINDNEXT, MA_FINDPREV, MA_REPLACE, MA_GOTO,
    MA_DUP, MA_DELLINE, MA_INDENT, MA_UNINDENT,
    MA_LINENUM, MA_WRAP, MA_TAB2, MA_TAB4, MA_TAB8, MA_SPACES, MA_STATUS, MA_SYNTAX,
    MA_LANG0, MA_LANG1, MA_LANG2, MA_LANG3,
    MA_HELP, MA_ABOUT,
    MA_RECENT0 = 100
};

#define MAXI 28
static KMenuItem g_items[4][MAXI];
static int g_count[4];
static int g_acts[4][MAXI];
static char g_recent_label[NB_RECENT_MAX][64];

static void mitem(int m, const char *label, const char *shortcut, int act, bool disabled, bool checked) {
    if (g_count[m] >= MAXI) return;
    KMenuItem *it = &g_items[m][g_count[m]];
    it->label = label;
    it->icon = checked ? KI_CHECK : (KIcon)0;
    it->separator = false;
    it->disabled = disabled;
    it->shortcut = shortcut;
    g_acts[m][g_count[m]++] = act;
}

static void msep(int m) {
    if (g_count[m] >= MAXI) return;
    KMenuItem *it = &g_items[m][g_count[m]];
    memset(it, 0, sizeof *it);
    it->label = "";                                             /* k_menubar measures every label, separators included */
    it->separator = true;
    g_acts[m][g_count[m]++] = MA_NONE;
}

static void short_path(const char *path, char *out, size_t cap) {
    const char *home = getenv("HOME");
    char tmp[NB_PATH_MAX + 4];
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || path[hl] == 0)) snprintf(tmp, sizeof tmp, "~%s", path + hl);
    else snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n > 40) snprintf(out, cap, "...%s", tmp + n - 37);
    else snprintf(out, cap, "%s", tmp);
}

static void build_menus(void) {
    memset(g_count, 0, sizeof g_count);
    Editor *e = &A.ed;
    bool sel = ed_has_selection(e);
    bool ro = A.readonly;
    /* File */
    mitem(0, "New", "Ctrl+N", MA_NEW, false, false);
    mitem(0, "New Window", "Ctrl+Shift+N", MA_NEWWIN, false, false);
    mitem(0, "Open...", "Ctrl+O", MA_OPEN, false, false);
    mitem(0, "Save", "Ctrl+S", MA_SAVE, false, false);
    mitem(0, "Save As...", "Ctrl+Shift+S", MA_SAVEAS, false, false);
    mitem(0, "Close", "Ctrl+W", MA_CLOSE, false, false);
    mitem(0, "Quit", "Ctrl+Q", MA_QUIT, false, false);
    int avail = (A.mainui ? A.mainui->cv.h : 400) - 26 - 6;       /* the drop-down must fit into the window */
    int room = avail - (4 + 22 * 7);
    int nshow = 0;
    if (A.nrecent > 0 && room >= 7 + 22 + 7 + 22) nshow = (room - 7 - 7 - 22) / 22 < A.nrecent ? (room - 7 - 7 - 22) / 22 : A.nrecent;
    if (nshow > 0) {
        msep(0);
        for (int i = 0; i < nshow; i++) {
            char sp[64];
            short_path(A.recent[i], sp, sizeof sp);
            snprintf(g_recent_label[i], sizeof g_recent_label[i], "%d  %s", i + 1, sp);
            mitem(0, g_recent_label[i], NULL, MA_RECENT0 + i, false, false);
        }
        msep(0);
        mitem(0, "Clear Recent Files", NULL, MA_CLEAR_RECENT, false, false);
    }
    /* Edit */
    mitem(1, "Undo", "Ctrl+Z", MA_UNDO, ro || !doc_can_undo(&e->doc), false);
    mitem(1, "Redo", "Ctrl+Y", MA_REDO, ro || !doc_can_redo(&e->doc), false);
    msep(1);
    mitem(1, "Cut", "Ctrl+X", MA_CUT, ro || !sel, false);
    mitem(1, "Copy", "Ctrl+C", MA_COPY, !sel, false);
    mitem(1, "Paste", "Ctrl+V", MA_PASTE, ro, false);
    mitem(1, "Select All", "Ctrl+A", MA_SELALL, false, false);
    msep(1);
    mitem(1, "Find...", "Ctrl+F", MA_FIND, false, false);
    mitem(1, "Find Next", "F3", MA_FINDNEXT, false, false);
    mitem(1, "Find Previous", "Shift+F3", MA_FINDPREV, false, false);
    mitem(1, "Replace...", "Ctrl+H", MA_REPLACE, false, false);
    mitem(1, "Go to Line...", "Ctrl+G", MA_GOTO, false, false);
    msep(1);
    mitem(1, "Indent", "Tab", MA_INDENT, ro, false);
    mitem(1, "Unindent", "Shift+Tab", MA_UNINDENT, ro, false);
    mitem(1, "Duplicate Line", "Ctrl+Shift+D", MA_DUP, ro, false);
    mitem(1, "Delete Line", "Ctrl+Shift+K", MA_DELLINE, ro, false);
    /* View */
    mitem(2, "Line Numbers", NULL, MA_LINENUM, false, e->numbers);
    mitem(2, "Word Wrap", NULL, MA_WRAP, false, e->wrap);
    mitem(2, "Status Bar", NULL, MA_STATUS, false, A.show_status);
    mitem(2, "Syntax Colours", NULL, MA_SYNTAX, false, e->highlight);
    msep(2);
    mitem(2, "Tab Width 2", NULL, MA_TAB2, false, e->tab_w == 2);
    mitem(2, "Tab Width 4", NULL, MA_TAB4, false, e->tab_w == 4);
    mitem(2, "Tab Width 8", NULL, MA_TAB8, false, e->tab_w == 8);
    mitem(2, "Tab Key Inserts Spaces", NULL, MA_SPACES, false, e->spaces);
    msep(2);
    mitem(2, "Plain Text", NULL, MA_LANG0, false, e->lang == LANG_PLAIN);
    mitem(2, "C", NULL, MA_LANG1, false, e->lang == LANG_C);
    mitem(2, "Shell", NULL, MA_LANG2, false, e->lang == LANG_SHELL);
    mitem(2, "Markdown", NULL, MA_LANG3, false, e->lang == LANG_MARKDOWN);
    /* Help */
    mitem(3, "Keyboard Shortcuts", "F1", MA_HELP, false, false);
    mitem(3, "About Patchouli's Notebook", NULL, MA_ABOUT, false, false);
}

static void set_lang(Lang l) {
    A.ed.lang = l;
    ed_invalidate(&A.ed);
}

static void do_action(int a) {
    Editor *e = &A.ed;
    if (a >= MA_RECENT0 && a < MA_RECENT0 + NB_RECENT_MAX) {
        char path[NB_PATH_MAX];
        snprintf(path, sizeof path, "%s", A.recent[a - MA_RECENT0]);
        guard(ACT_OPEN, path);
        return;
    }
    switch (a) {
    case MA_NEW: act_new(); break;
    case MA_NEWWIN: spawn_window(NULL); break;
    case MA_OPEN: act_open(); break;
    case MA_SAVE: act_save(); break;
    case MA_SAVEAS: act_save_as(); break;
    case MA_CLOSE: act_close(); break;
    case MA_QUIT: act_quit(); break;
    case MA_CLEAR_RECENT: recent_clear(); break;
    case MA_UNDO: ed_undo(e); break;
    case MA_REDO: ed_redo(e); break;
    case MA_CUT: ed_copy(e, true); break;
    case MA_COPY: ed_copy(e, false); break;
    case MA_PASTE: ed_paste(e); break;
    case MA_SELALL: ed_select_all(e); break;
    case MA_FIND: find_open(false); break;
    case MA_FINDNEXT: if (A.find_buf[0]) find_next(false, false); else find_open(false); break;
    case MA_FINDPREV: if (A.find_buf[0]) find_next(true, false); else find_open(false); break;
    case MA_REPLACE: find_open(true); break;
    case MA_GOTO: act_goto(); break;
    case MA_INDENT: ed_indent(e, false); break;
    case MA_UNINDENT: ed_indent(e, true); break;
    case MA_DUP: ed_duplicate_line(e); break;
    case MA_DELLINE: ed_delete_line(e); break;
    case MA_LINENUM: e->numbers = !e->numbers; config_save(); break;
    case MA_WRAP: e->wrap = !e->wrap; e->top_sub = 0; e->scroll_x = 0; ed_invalidate(e); e->reveal = true; config_save(); break;
    case MA_STATUS: A.show_status = !A.show_status; config_save(); break;
    case MA_SYNTAX: e->highlight = !e->highlight; config_save(); break;
    case MA_TAB2: e->tab_w = 2; ed_invalidate(e); config_save(); break;
    case MA_TAB4: e->tab_w = 4; ed_invalidate(e); config_save(); break;
    case MA_TAB8: e->tab_w = 8; ed_invalidate(e); config_save(); break;
    case MA_SPACES: A.pref_spaces = !A.pref_spaces; e->spaces = A.pref_spaces; config_save(); break;
    case MA_LANG0: set_lang(LANG_PLAIN); break;
    case MA_LANG1: set_lang(LANG_C); break;
    case MA_LANG2: set_lang(LANG_SHELL); break;
    case MA_LANG3: set_lang(LANG_MARKDOWN); break;
    case MA_HELP: dlg_help(A.app); break;
    case MA_ABOUT: dlg_about(A.app); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- shortcuts */

static void mb_open(KUI *ui, int m);

static bool app_shortcut(KUI *ui, KKey *k) {
    bool ctrl = (k->mods & MOD_CTRL) != 0, shift = (k->mods & MOD_SHIFT) != 0, alt = (k->mods & MOD_ALT) != 0;
    if (ctrl && !alt) {
        switch (k->code) {
        case KEY_N: if (shift) spawn_window(NULL); else act_new(); return true;
        case KEY_O: act_open(); return true;
        case KEY_S: if (shift) act_save_as(); else act_save(); return true;
        case KEY_W: act_close(); return true;
        case KEY_Q: act_quit(); return true;
        case KEY_F: find_open(false); return true;
        case KEY_H: find_open(true); return true;
        case KEY_G: act_goto(); return true;
        default: break;
        }
    }
    if (!ctrl && !alt) {
        if (k->code == KEY_F3) {
            if (A.find_buf[0]) find_next(shift, false); else find_open(false);
            return true;
        }
        if (k->code == KEY_F1) { dlg_help(A.app); return true; }
        if (k->code == KEY_F10) { mb_open(ui, ui->menu_open ? 0 : 1); return true; }
        if (k->code == KEY_ESC && A.find_open && !ui->menu_open) { find_close(); return true; }
    }
    if (alt && !ctrl && !shift) {
        int m = k->code == KEY_F ? 1 : k->code == KEY_E ? 2 : k->code == KEY_V ? 3 : k->code == KEY_H ? 4 : 0;
        if (m) { mb_open(ui, ui->menu_open == m ? 0 : m); return true; }
    }
    return false;
}

/* ---------------------------------------------------------------- status bar */

static void update_title(void) {
    char name[64];
    if (strlen(A.name) > 36) snprintf(name, sizeof name, "%.33s...", A.name); else snprintf(name, sizeof name, "%s", A.name);
    char t[160];
    snprintf(t, sizeof t, "Notebook - %s%s%s", is_modified() ? "*" : "", name, A.readonly ? " (read-only)" : "");
    if (strcmp(t, A.title)) {
        snprintf(A.title, sizeof A.title, "%s", t);
        kapp_set_title(A.app, t);
        nb_trace("title %s", t);
    }
}

static void draw_status(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    kc_fill(cv, r, th->window_bg);
    kc_hline(cv, r.x, r.y, r.w, kc_shade(th->window_bg, 72));
    kc_hline(cv, r.x, r.y + 1, r.w, 0xFFFFFF);
    int ty = r.y + 4;
    int line, col;
    ed_caret_pos(&A.ed, &line, &col);
    size_t lo, hi;
    ed_selection(&A.ed, &lo, &hi);
    char pos[96];
    int n = snprintf(pos, sizeof pos, "Ln %d, Col %d", line + 1, col + 1);
    if (hi > lo) snprintf(pos + n, sizeof pos - (size_t)n, "  (%zu selected)", hi - lo);
    int px = r.x + 8;
    kc_text(cv, px, ty, pos, th->text);
    int left_end = px + kc_text_width(pos) + 16;
    if (k_hover(ui, kr(r.x, r.y, left_end, r.h))) k_tooltip_text(ui, A.has_path ? A.path : "This page has no file yet");

    char sz[40], lines[32], eol[24];
    fmt_size(sz, sizeof sz, saved_size());
    snprintf(lines, sizeof lines, "%d line%s", doc_nlines(&A.ed.doc), doc_nlines(&A.ed.doc) == 1 ? "" : "s");
    snprintf(eol, sizeof eol, "%s%s", nb_eol_name(A.eol), A.eol_mixed ? "*" : "");
    struct { const char *text; int w; int act; const char *tip; } seg[5] = {
        { syn_name(A.ed.lang), 0, 1, "Language for the colours: click to change" },
        { eol, 0, 2, "Line endings: click to switch between LF and CRLF" },
        { A.utf8_ok ? "UTF-8" : "UTF-8?", 0, 0, A.utf8_ok ? "Text encoding" : "Some bytes are not valid UTF-8; they are kept as they are" },
        { sz, 0, 0, "Size of the saved file" },
        { lines, 0, 0, "Number of lines" },
    };
    /* segments from the right; lines and size are dropped first when the bar is narrow */
    int x = r.x + r.w - 4;
    int shown[5] = { 0 };
    int order[5] = { 1, 2, 0, 3, 4 };                           /* priority: line endings, encoding, language, size, lines */
    int minroom = A.msg[0] ? kc_text_width(A.msg) + 24 : 100;   /* a message is more important than the size and line count */
    if (minroom > (x - left_end) * 3 / 4) minroom = (x - left_end) * 3 / 4;
    int room = x - left_end - minroom;
    int used = 0;
    for (int i = 0; i < 5; i++) {
        int s = order[i];
        int w = kc_text_width(seg[s].text) + 20;
        if (used + w > room) continue;
        used += w;
        shown[s] = 1;
        seg[s].w = w;
    }
    int right_start = x;
    for (int s = 0; s < 5; s++) {                               /* draw from the right: lang (0) is the rightmost */
        if (!shown[s]) continue;
        KRect sr = kr(x - seg[s].w, r.y + 2, seg[s].w, r.h - 2);
        bool hover = seg[s].act && k_hover(ui, sr);
        if (hover) { kc_bevel(cv, sr, KB_RAISED); k_tooltip_text(ui, seg[s].tip); }
        else if (k_hover(ui, sr)) k_tooltip_text(ui, seg[s].tip);
        kc_text_in(cv, sr, seg[s].text, th->text, KA_CENTER);
        kc_vline(cv, sr.x, r.y + 4, r.h - 6, kc_shade(th->window_bg, 78));
        if (seg[s].act && k_clicked(ui, sr)) {
            if (seg[s].act == 1) set_lang((Lang)((A.ed.lang + 1) % LANG_COUNT));
            else if (seg[s].act == 2 && !A.readonly) {
                A.eol = A.eol == EOL_LF ? EOL_CRLF : EOL_LF;
                A.eol_mixed = false;
                A.ed.doc.save_lost = true;                        /* the file would now differ from what is on disk: count as modified */
                msg_set("Line endings will be %s when you save.", nb_eol_name(A.eol));
            }
        }
        x -= seg[s].w;
    }
    right_start = x;
    if (A.msg[0]) {
        KRect mr = kr(left_end, ty - 2, right_start - left_end - 6, 20);
        if (mr.w > 16) kc_text_in(cv, mr, A.msg, th->accent_dark, KA_LEFT);
    } else if (A.readonly) {
        KRect mr = kr(left_end, ty - 2, right_start - left_end - 6, 20);
        if (mr.w > 16) kc_text_in(cv, mr, "Read-only", th->accent_dark, KA_LEFT);
    }
}

/* ---------------------------------------------------------------- menu bar (mouse and keyboard) */

/* The stock k_menubar only knows the mouse. This one draws the same bar and drop-downs but also takes Alt+letter, the arrow
 * keys, Home/End, Enter and Esc. ui->menu_open is the open menu (1-based), ui->menu_item_hover the highlighted item (-1: none). */

static const char *const g_mb_titles[4] = { "File", "Edit", "View", "Help" };
static int g_mb_act, g_mb_lastx = -1, g_mb_lasty = -1;

static void mb_step(KUI *ui, int dir) {
    int open = ui->menu_open;
    if (!open) return;
    int n = g_count[open - 1], i = ui->menu_item_hover;
    for (int tries = 0; tries < n; tries++) {
        i = i < 0 || i >= n ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (!g_items[open - 1][i].separator && !g_items[open - 1][i].disabled) { ui->menu_item_hover = i; return; }
    }
}

static void mb_open(KUI *ui, int m) {
    ui->menu_open = m;
    ui->menu_item_hover = -1;
}

/* Returns true when the key belongs to the open menu (plain keys are swallowed while a menu is open). */
static bool mb_key(KUI *ui, const KKey *k) {
    int open = ui->menu_open;
    if (!open || (k->mods & (MOD_CTRL | MOD_ALT))) return false;
    int n = g_count[open - 1];
    switch (k->code) {
    case KEY_ESC: mb_open(ui, 0); break;
    case KEY_LEFT: mb_open(ui, open == 1 ? 4 : open - 1); break;
    case KEY_RIGHT: mb_open(ui, open == 4 ? 1 : open + 1); break;
    case KEY_DOWN: mb_step(ui, 1); break;
    case KEY_UP: mb_step(ui, -1); break;
    case KEY_HOME: ui->menu_item_hover = -1; mb_step(ui, 1); break;
    case KEY_END: ui->menu_item_hover = -1; mb_step(ui, -1); break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        if (ui->menu_item_hover >= 0 && ui->menu_item_hover < n) { g_mb_act = g_acts[open - 1][ui->menu_item_hover]; mb_open(ui, 0); }
        break;
    default: break;
    }
    return true;
}

/* Handles the pointer, draws the bar and the open drop-down; returns the chosen action (0: none). live: accept the pointer. */
static int mb_run(KUI *ui, KRect bar, bool live) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    int act = g_mb_act;
    g_mb_act = 0;
    KRect tr[4];
    int x = bar.x + 2;
    for (int m = 0; m < 4; m++) {
        int w = kc_text_width(g_mb_titles[m]) + 16;
        tr[m] = kr(x, bar.y + 1, w, bar.h - 2);
        x += w;
    }
    bool mouse = live && ui->mouse_inside;
    bool moved = ui->mx != g_mb_lastx || ui->my != g_mb_lasty;
    g_mb_lastx = ui->mx;
    g_mb_lasty = ui->my;
    int open = ui->menu_open;
    bool over_bar = false;
    for (int m = 0; m < 4; m++) {
        bool hv = mouse && kr_contains(tr[m], ui->mx, ui->my);
        if (hv) over_bar = true;
        if (hv && ui->pressed[0]) { open = open == m + 1 ? 0 : m + 1; ui->menu_item_hover = -1; }
        else if (hv && open && open != m + 1 && moved) { open = m + 1; ui->menu_item_hover = -1; }
    }
    ui->menu_open = open;
    KRect box = kr(0, 0, 0, 0);
    KRect irs[MAXI];
    if (open) {
        const KMenuItem *items = g_items[open - 1];
        int n = g_count[open - 1], w = 0, h = 4;
        for (int i = 0; i < n; i++) {
            int iw = kc_text_width(items[i].label) + 36 + (items[i].shortcut ? kc_text_width(items[i].shortcut) + 24 : 0);
            if (iw > w) w = iw;
            h += items[i].separator ? 7 : 22;
        }
        box = kr(tr[open - 1].x, bar.y + bar.h + 1, w + 4, h);
        int y = box.y + 2, over = -1;
        for (int i = 0; i < n; i++) {
            irs[i] = kr(box.x + 2, y, box.w - 4, items[i].separator ? 5 : 22);
            y += items[i].separator ? 7 : 22;
            if (mouse && !items[i].separator && !items[i].disabled && kr_contains(irs[i], ui->mx, ui->my)) over = i;
        }
        if (over >= 0 && moved) ui->menu_item_hover = over;
        if (live && ui->released[0] && over >= 0) { act = g_acts[open - 1][over]; ui->menu_open = 0; }
        else if (live && ui->pressed[0] && !kr_contains(box, ui->mx, ui->my) && !over_bar) { ui->menu_open = 0; ui->menu_item_hover = -1; }
        if (act) ui->menu_open = 0;
    }
    /* the bar */
    kc_fill(cv, bar, th->window_bg);
    kc_hline(cv, bar.x, bar.y + bar.h - 1, bar.w, kc_shade(th->window_bg, 72));
    kc_hline(cv, bar.x, bar.y + bar.h, bar.w, 0xFFFFFF);
    bool underline = (ui->mods & MOD_ALT) != 0 || ui->menu_open;
    for (int m = 0; m < 4; m++) {
        bool hv = mouse && kr_contains(tr[m], ui->mx, ui->my);
        if (ui->menu_open == m + 1) kc_panel(cv, tr[m], KB_SUNKEN, kc_shade(th->window_bg, 94));
        else if (hv) kc_bevel(cv, tr[m], KB_RAISED);
        kc_text_in(cv, tr[m], g_mb_titles[m], th->text, KA_CENTER);
        if (underline) kc_hline(cv, tr[m].x + (tr[m].w - kc_text_width(g_mb_titles[m])) / 2, tr[m].y + 17, 8, th->text);
    }
    /* the drop-down, only if it is still open after the pointer was handled (otherwise it would linger on screen) */
    if (ui->menu_open && ui->menu_open == open) {
        const KMenuItem *items = g_items[open - 1];
        int n = g_count[open - 1];
        kc_fill(cv, kr(box.x + 3, box.y + 3, box.w, box.h), kc_shade(th->window_bg, 55));
        kc_panel(cv, box, KB_RAISED, th->window_bg);
        for (int i = 0; i < n; i++) {
            if (items[i].separator) { k_separator(ui, kr(box.x + 6, irs[i].y + 2, box.w - 12, 2)); continue; }
            bool hot = !items[i].disabled && ui->menu_item_hover == i;
            if (hot) kc_fill(cv, irs[i], th->select_bg);
            uint32_t fg = items[i].disabled ? th->text_dim : hot ? th->select_text : th->text;
            if (items[i].icon) k_icon(cv, items[i].icon, irs[i].x + 4, irs[i].y + 3, 16);
            kc_text(cv, irs[i].x + 26, irs[i].y + 3, items[i].label, fg);
            if (items[i].shortcut) kc_text(cv, irs[i].x + irs[i].w - 8 - kc_text_width(items[i].shortcut), irs[i].y + 3, items[i].shortcut, fg);
        }
    }
    return act;
}

/* ---------------------------------------------------------------- context menu (right click in the text) */

typedef struct { const char *label, *keys; int act; bool sep; } CxItem;
static const CxItem g_cx_items[] = {
    { "Undo", "Ctrl+Z", MA_UNDO, false }, { "Redo", "Ctrl+Y", MA_REDO, false }, { "", NULL, 0, true },
    { "Cut", "Ctrl+X", MA_CUT, false }, { "Copy", "Ctrl+C", MA_COPY, false }, { "Paste", "Ctrl+V", MA_PASTE, false }, { "", NULL, 0, true },
    { "Select All", "Ctrl+A", MA_SELALL, false },
};
#define CX_N ((int)(sizeof g_cx_items / sizeof g_cx_items[0]))
static struct { bool open; int x, y, w, h, hover; int key_act; } g_cx;

static bool cx_enabled(int act) {
    bool sel = ed_has_selection(&A.ed);
    switch (act) {
    case MA_UNDO: return !A.readonly && doc_can_undo(&A.ed.doc);
    case MA_REDO: return !A.readonly && doc_can_redo(&A.ed.doc);
    case MA_CUT: return !A.readonly && sel;
    case MA_COPY: return sel;
    case MA_PASTE: return !A.readonly;
    default: return true;
    }
}

static void cx_open(KUI *ui, int W, int H) {
    g_cx.open = true;
    g_cx.w = 200;
    g_cx.h = 4;
    for (int i = 0; i < CX_N; i++) g_cx.h += g_cx_items[i].sep ? 7 : 22;
    g_cx.x = ui->mx;
    g_cx.y = ui->my;
    if (g_cx.x + g_cx.w > W) g_cx.x = W - g_cx.w;
    if (g_cx.y + g_cx.h > H) g_cx.y = H - g_cx.h;
    if (g_cx.x < 0) g_cx.x = 0;
    if (g_cx.y < 0) g_cx.y = 0;
    g_cx.hover = -1;
    g_cx.key_act = 0;
    nb_trace("context menu");
}

static void cx_step(int dir) {
    int i = g_cx.hover;
    for (int tries = 0; tries < CX_N; tries++) {
        i = (i + dir + CX_N) % CX_N;
        if (i < 0) i = CX_N - 1;
        if (!g_cx_items[i].sep && cx_enabled(g_cx_items[i].act)) { g_cx.hover = i; return; }
    }
}

static void cx_key(const KKey *k) {
    if (k->code == KEY_ESC) g_cx.open = false;
    else if (k->code == KEY_DOWN) cx_step(1);
    else if (k->code == KEY_UP) cx_step(-1);
    else if ((k->code == KEY_ENTER || k->code == KEY_KPENTER) && g_cx.hover >= 0) g_cx.key_act = g_cx_items[g_cx.hover].act;
}

/* Handles the pointer and draws the menu; returns the chosen action (or 0). */
static int cx_run(KUI *ui) {
    if (!g_cx.open) return 0;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect box = kr(g_cx.x, g_cx.y, g_cx.w, g_cx.h);
    int act = g_cx.key_act;
    g_cx.key_act = 0;
    int y = box.y + 2, over = -1;
    KRect rects[CX_N];
    for (int i = 0; i < CX_N; i++) {
        rects[i] = kr(box.x + 2, y, box.w - 4, g_cx_items[i].sep ? 5 : 22);
        y += g_cx_items[i].sep ? 7 : 22;
        if (!g_cx_items[i].sep && ui->mouse_inside && kr_contains(rects[i], ui->mx, ui->my)) over = i;
    }
    if (over >= 0 && (ui->mx != g_cx.x || ui->my != g_cx.y)) g_cx.hover = over;
    bool close = act != 0;
    if (ui->released[0] && over >= 0 && cx_enabled(g_cx_items[over].act)) { act = g_cx_items[over].act; close = true; }
    else if ((ui->pressed[0] || ui->pressed[1] || ui->pressed[2]) && !kr_contains(box, ui->mx, ui->my)) close = true;
    if (!g_cx.open) close = true;
    if (close) {
        g_cx.open = false;
        kapp_redraw(A.app);
        return act;
    }
    kc_fill(cv, kr(box.x + 3, box.y + 3, box.w, box.h), kc_shade(th->window_bg, 55));
    kc_panel(cv, box, KB_RAISED, th->window_bg);
    for (int i = 0; i < CX_N; i++) {
        const CxItem *it = &g_cx_items[i];
        if (it->sep) { k_separator(ui, kr(rects[i].x + 4, rects[i].y + 1, rects[i].w - 8, 2)); continue; }
        bool en = cx_enabled(it->act), hot = en && g_cx.hover == i;
        if (hot) kc_fill(cv, rects[i], th->select_bg);
        uint32_t fg = !en ? th->text_dim : hot ? th->select_text : th->text;
        kc_text(cv, rects[i].x + 12, rects[i].y + 3, it->label, fg);
        if (it->keys) kc_text(cv, rects[i].x + rects[i].w - 8 - kc_text_width(it->keys), rects[i].y + 3, it->keys, fg);
    }
    return 0;
}

/* ---------------------------------------------------------------- the window */

static void trace_state(void) {
    if (!A.trace) return;
    static int pl = -1, pc = -1, pm = -1, pn = -1;
    static size_t ps = (size_t)-1;
    int line, col;
    ed_caret_pos(&A.ed, &line, &col);
    size_t lo, hi;
    ed_selection(&A.ed, &lo, &hi);
    int m = is_modified(), nl = doc_nlines(&A.ed.doc);
    if (line != pl || col != pc || m != pm || nl != pn || hi - lo != ps) {
        pl = line; pc = col; pm = m; pn = nl; ps = hi - lo;
        nb_trace("pos %d %d mod=%d lines=%d sel=%zu", line + 1, col + 1, m, nl, hi - lo);
    }
    static int ptop = -1;
    if (A.ed.top_line != ptop) { ptop = A.ed.top_line; nb_trace("top %d", ptop + 1); }
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    A.in_draw = true;
    A.mainui = ui;
    uint64_t t0 = k_now_ms();
    KCanvas *cv = &ui->cv;
    int W = cv->w, H = cv->h;
    k_clear(ui);
    if (!A.focus_init) { ui->focus = ED_ID; ui->menu_item_hover = -1; A.focus_init = true; }
    if (A.want_editor_focus) { ui->focus = ED_ID; A.want_editor_focus = false; }
    find_apply_focus(ui);
    reap_children();
    if (A.msg[0] && ui->now_ms > A.msg_until) A.msg[0] = 0;
    else if (A.msg[0]) ui->redraw_soon = true;

    bool modal = A.modal > 0;
    if (modal) ui->input_blocked = true;

    int mb_h = 24;
    int sb_h = A.show_status ? 22 : 0;
    int fb_h = findbar_height();
    KRect edr = kr(0, mb_h + 2, W, H - mb_h - 2 - sb_h - fb_h);
    if (edr.h < 40) edr.h = 40;
    KRect fbr = kr(0, edr.y + edr.h, W, fb_h);
    KRect sbr = kr(0, H - sb_h, W, sb_h);

    /* keys, in the order they arrived: application shortcuts first, the rest goes to the editor */
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        if (modal) { k->down = 0; k->code = 0; continue; }
        if (g_cx.open) { cx_key(k); k->down = 0; k->code = 0; continue; }
        if (ui->menu_open && mb_key(ui, k)) { k->down = 0; k->code = 0; continue; }
        if (app_shortcut(ui, k)) { k->down = 0; k->code = 0; continue; }
        if (ui->focus == ED_ID && !ui->menu_open && ed_key(&A.ed, ui, k)) { k->down = 0; k->code = 0; }
    }

    /* a right click in the text opens the context menu (and moves the caret there unless it hit the selection) */
    if (!modal && !g_cx.open && !ui->menu_open && ui->pressed[1] && ui->mouse_inside && kr_contains(edr, ui->mx, ui->my) &&
        ui->mx < edr.x + edr.w - 16) {
        size_t off = ed_offset_at(&A.ed, ui->mx, ui->my), lo, hi;
        ed_selection(&A.ed, &lo, &hi);
        if (off < lo || off > hi) ed_set_selection(&A.ed, off, off);
        ui->focus = ED_ID;
        cx_open(ui, W, H);
    }

    bool inside = ui->mouse_inside;
    if (g_cx.open) ui->input_blocked = true;                    /* the menu owns the pointer while it is open */
    bool blocked = modal || ui->input_blocked;
    ed_draw(&A.ed, ui, edr, ui->focus == ED_ID, blocked);
    findbar_draw(ui, fbr);
    if (A.show_status) draw_status(ui, sbr);
    if (ui->focus == 0 && !ui->menu_open && !modal) ui->focus = ED_ID;     /* a click on a button must not leave the keyboard nowhere */

    /* menu bar last: its drop-down paints over everything */
    build_menus();
    int menu_before = ui->menu_open;
    int pick = mb_run(ui, kr(0, 0, W, mb_h), !modal && !g_cx.open);
    if (A.trace) {
        static int last_menu = 0, last_item = -2;
        if (ui->menu_open != last_menu || (ui->menu_open && ui->menu_item_hover != last_item)) {
            last_menu = ui->menu_open;
            last_item = ui->menu_item_hover;
            nb_trace("menu %d item %d", ui->menu_open, ui->menu_open ? ui->menu_item_hover : -1);
        }
    }
    if (menu_before && !ui->menu_open) kapp_redraw(A.app);        /* make sure the closed state is painted */
    if (pick) {
        do_action(pick);
        if (A.want_editor_focus) { ui->focus = ED_ID; A.want_editor_focus = false; }
        if (!A.find_open) ui->focus = ED_ID;
    }
    if (g_cx.open) {
        ui->mouse_inside = inside;
        int cact = cx_run(ui);
        if (cact) { do_action(cact); if (!A.find_open) ui->focus = ED_ID; }
    }
    update_title();
    trace_state();
    A.in_draw = false;
    A.frame_ms = k_now_ms() - t0;
    if (A.trace && A.frame_ms > 150) nb_trace("slow-frame %llu ms", (unsigned long long)A.frame_ms);
}

static bool on_close(KApp *app, void *user) {
    (void)app; (void)user;
    if (A.modal > 0) return false;
    if (!is_modified()) { A.quitting = true; nb_trace("quit"); return true; }
    guard(ACT_QUIT, NULL);
    return false;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest_run();
    A.trace = getenv("NOTEBOOK_TRACE") != NULL;
    A.show_status = true;
    A.pref_spaces = true;
    ed_init(&A.ed);
    config_load();
    A.pref_spaces = A.ed.spaces;
    recent_load();
    A.app = kapp_new("Notebook", 720, 520, FW_RESIZABLE);
    if (!A.app) return 1;
    kapp_set_draw(A.app, draw, NULL);
    kapp_set_close_handler(A.app, on_close, NULL);
    kapp_set_minsize(A.app, 480, 240);
    reset_empty();
    nb_trace("start");
    if (argc > 1 && argv[1][0]) load_path(argv[1], false);
    int rc = kapp_run(A.app);
    kapp_destroy(A.app);
    free(A.clip);
    ed_free(&A.ed);
    return rc;
}
