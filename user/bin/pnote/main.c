/*
 * pnote: Patchouli's Notebook. Command line, key bindings and the main loop.
 *
 *   pnote [options] [+LINE[,COL]] [FILE]      edit FILE (created when saved if it does not exist)
 *   pnote -                                   edit what arrives on standard input; when standard output is a pipe or a
 *                                             file too, leaving the editor sends the text on (like vipe)
 */
#include "pnote.h"
#include <getopt.h>

static int g_discard;               /* filter mode: the user threw the text away */

static void usage(FILE *f) {
    fprintf(f,
        "usage: pnote [options] [+LINE[,COL]] [FILE | -]\n"
        "  -n, --linenumbers      show line numbers\n"
        "  -S, --nohighlight      no syntax highlighting\n"
        "  -I, --noindent         no auto-indent\n"
        "  -E, --tabstospaces     the Tab key inserts spaces\n"
        "  -T, --tabsize=N        tab width (default 8)\n"
        "  -R, --readonly         view only: refuse all edits\n"
        "  -l, --language=LANG    highlight as c, sh, md or none (default: by file name)\n"
        "  -V, --version          print the version\n"
        "  -h, --help             this help\n"
        "A file name of - reads standard input. Inside the editor ^G shows the key bindings.\n");
}

/* ------------------------------------------------------------------ saving and leaving */

static int write_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = buf_write_fd(fd);
    int e = errno;
    if (close(fd) < 0 && rc == 0) { rc = -1; e = errno; }
    errno = e;
    return rc;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Saves the buffer; with ask_name (or when it has no name yet) asks for a file name first. Returns 1 on success. */
int save_buffer(int ask_name) {
    char name[512] = "";
    for (;;) {
        if (!E.path || ask_name) {
            char label[64] = "File name to write: ";
            if (E.path) snprintf(name, sizeof name, "%s", E.path);
            if (!prompt_line(label, sizeof label, name, sizeof name, NULL) || !name[0]) { set_msg("Cancelled"); return 0; }
            if ((!E.path || strcmp(name, E.path)) && file_exists(name)) {
                int k = ask_key("That file exists. Overwrite it?  Y yes  N no", "yn");
                if (k != 'y') { set_msg("Cancelled"); return 0; }
            }
        } else snprintf(name, sizeof name, "%s", E.path);
        if (write_file(name) == 0) break;
        int e = errno;
        set_msg("Cannot write %s: %s", name, strerror(e));
        int k = ask_key("Writing failed. Save under another name?  Y yes  N no", "yn");
        if (k != 'y') return 0;
        ask_name = 1;
    }
    free(E.path);
    E.path = strdup(name);
    E.new_file = 0;
    E.file_readonly = 0;
    E.was_empty = 0;
    buf_mark_saved();
    if (E.lang_auto && E.lang == LANG_NONE) {
        E.lang = hl_detect(E.path, E.n ? LN(0)->s : NULL, E.n ? LN(0)->len : 0);
        hl_invalidate(0);
    }
    set_msg("Wrote %ld line%s (%zu bytes) to %s", E.n, E.n == 1 ? "" : "s", buf_total_bytes(), E.path);
    return 1;
}

void do_exit(void) {
    if (E.filter) {
        if (buf_modified()) {
            int k = ask_key("Send the edited text down the pipe?  Y yes  N no, throw it away  Esc stay", "yn");
            if (k == 0) return;
            if (k == 'n') g_discard = 1;
        }
        E.quit = 1;
        return;
    }
    if (buf_modified()) {
        int k = ask_key("Save modified buffer?  Y yes  N no  Esc cancel", "yn");
        if (k == 0) { set_msg("Cancelled"); return; }
        if (k == 'y' && !save_buffer(0)) return;
    }
    E.quit = 1;
}

/* ------------------------------------------------------------------ other commands */

static void cmd_insert_file(void) {
    if (!edit_allowed()) return;
    char label[64] = "Insert file: ", name[512] = "";
    if (!prompt_line(label, sizeof label, name, sizeof name, NULL) || !name[0]) return;
    int fd = open(name, O_RDONLY);
    if (fd < 0) { set_msg("Cannot read %s: %s", name, strerror(errno)); return; }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); set_msg("%s is a directory", name); return; }
    PtBuf data = { 0 };
    char tmp[8192];
    ssize_t r;
    while ((r = read(fd, tmp, sizeof tmp)) > 0) pb_add(&data, tmp, (size_t)r);
    close(fd);
    if (data.len == 0) { set_msg("%s is empty", name); pb_free(&data); return; }
    undo_boundary();
    insert_text_at_cursor(data.p, data.len);
    set_msg("Inserted %zu bytes from %s", data.len, name);
    pb_free(&data);
}

static void cmd_goto_prompt(void) {
    char label[64] = "Go to line[,column]: ", text[64] = "";
    if (!prompt_line(label, sizeof label, text, sizeof text, NULL) || !text[0]) return;
    long l = 0, c = 0;
    int n = sscanf(text, "%ld,%ld", &l, &c);
    if (n < 1) { set_msg("Not a line number: %s", text); return; }
    if (l < 0) l = E.n + 1 + l;
    if (l < 1) l = 1;
    cmd_goto(l, n >= 2 ? c : 0);
    set_msg("Line %ld of %ld", E.cy + 1, E.n);
}

static void cmd_where(void) {
    size_t before = 0, total = 0;
    for (long i = 0; i < E.n; i++) {
        Line *L = LN(i);
        size_t sz = L->len + ((i < E.n - 1 || E.final_newline) ? (L->cr ? 2 : 1) : 0);
        if (i < E.cy) before += sz;
        total += sz;
    }
    before += E.cx;
    Line *L = LN(E.cy);
    set_msg("line %ld/%ld (%d%%), col %ld/%ld, byte %zu/%zu (%d%%)", E.cy + 1, E.n, (int)((E.cy + 1) * 100 / E.n),
            line_dispcol(L, E.cx) + 1, line_dispcol(L, L->len) + 1, before, total, total ? (int)(before * 100 / total) : 100);
}

void toggle_menu(void) {
    int k = ask_key("Toggle:  N numbers  H highlight  L language  A indent  T spaces  D CRLF  Esc", "nhlatd");
    switch (k) {
    case 'n': E.linenums = !E.linenums; set_msg("Line numbers %s", E.linenums ? "on" : "off"); break;
    case 'h':
        E.highlight = !E.highlight;
        if (E.highlight && E.lang == LANG_NONE) E.lang = hl_detect(E.path, E.n ? LN(0)->s : NULL, E.n ? LN(0)->len : 0);
        hl_invalidate(0);
        set_msg("Syntax highlighting %s%s", E.highlight ? "on" : "off", E.highlight && E.lang == LANG_NONE ? " (no language known: try L)" : "");
        break;
    case 'l':
        E.lang = (E.lang + 1) % LANG_COUNT;
        E.lang_auto = 0;
        E.highlight = E.lang != LANG_NONE;
        hl_invalidate(0);
        set_msg("Language: %s", hl_lang_name(E.lang));
        break;
    case 'a': E.autoindent = !E.autoindent; set_msg("Auto-indent %s", E.autoindent ? "on" : "off"); break;
    case 't': E.tabs_to_spaces = !E.tabs_to_spaces; set_msg("The Tab key inserts %s", E.tabs_to_spaces ? "spaces" : "tab characters"); break;
    case 'd': {
        if (!edit_allowed()) break;
        int to = E.n > 0 && !LN(0)->cr;
        for (long i = 0; i < E.n; i++) LN(i)->cr = (unsigned)to;
        E.force_modified = 1;
        set_msg("Line endings are now %s", to ? "CRLF (DOS)" : "LF (Unix)");
        break;
    }
    default: break;
    }
}

static void do_undo_cmd(void) {
    E.mark_on = 0;
    if (do_undo()) { clamp_cursor(); remember_col(); set_msg("Undone"); }
    else set_msg("Nothing to undo");
}

static void do_redo_cmd(void) {
    E.mark_on = 0;
    if (do_redo()) { clamp_cursor(); remember_col(); set_msg("Redone"); }
    else set_msg("Nothing to redo");
}

/* ------------------------------------------------------------------ key dispatch */

static void handle_alt(int k) {
    begin_cmd(K_OTHER);
    switch (k) {
    case 'a': cmd_toggle_mark(); break;
    case '6': case 'k': cmd_copy(); break;
    case 'u': do_undo_cmd(); break;
    case 'e': do_redo_cmd(); break;
    case 'b': cmd_word(-1); break;
    case 'f': cmd_word(1); break;
    case 'v': cmd_page(-1); break;
    case '\\': cmd_top(); break;
    case '/': cmd_bottom(); break;
    case 'w': cmd_search_again(1); break;
    case 'l': pts_invalidate(&E.scr); break;
    case PT_K_LEFT: cmd_word(-1); break;
    case PT_K_RIGHT: cmd_word(1); break;
    case PT_K_BACKSPACE: break;
    default: break;
    }
}

static void handle_key(int k) {
    E.msg[0] = 0;
    E.mh_on = 0;
    if (k & PT_M_ALT) { handle_alt(k & ~PT_M_ALT); return; }
    if (k >= PT_K_BASE && (k & PT_M_CTRL)) {
        begin_cmd(K_OTHER);
        switch (PT_KEY_BASE(k)) {
        case PT_K_LEFT: cmd_word(-1); break;
        case PT_K_RIGHT: cmd_word(1); break;
        case PT_K_HOME: cmd_top(); break;
        case PT_K_END: cmd_bottom(); break;
        case PT_K_UP: cmd_move_up(); break;
        case PT_K_DOWN: cmd_move_down(); break;
        default: break;
        }
        return;
    }
    if (k >= PT_K_BASE && (k & PT_M_SHIFT)) {                   /* Shift+movement starts a selection */
        int b = PT_KEY_BASE(k);
        if ((b == PT_K_LEFT || b == PT_K_RIGHT || b == PT_K_UP || b == PT_K_DOWN || b == PT_K_HOME || b == PT_K_END) && !E.mark_on) {
            E.mark_on = 1;
            E.mark_ln = E.cy;
            E.mark_col = E.cx;
        }
        k = b;
    }
    switch (k) {
    case PT_K_ENTER: begin_cmd(K_OTHER); cmd_newline(); break;
    case PT_K_TAB: begin_cmd(K_TYPE); cmd_tab(); break;
    case PT_K_BTAB: begin_cmd(K_OTHER); cmd_unindent(); break;
    case PT_K_BACKSPACE: begin_cmd(K_BS); cmd_backspace(); break;
    case PT_K_DEL: case PT_CTRL('d'): begin_cmd(K_DELCH); cmd_delete(); break;

    case PT_K_LEFT: case PT_CTRL('b'): begin_cmd(K_OTHER); cmd_move_left(); break;
    case PT_K_RIGHT: case PT_CTRL('f'): begin_cmd(K_OTHER); cmd_move_right(); break;
    case PT_K_UP: begin_cmd(K_OTHER); cmd_move_up(); break;
    case PT_K_DOWN: begin_cmd(K_OTHER); cmd_move_down(); break;
    case PT_K_HOME: case PT_CTRL('a'): begin_cmd(K_OTHER); cmd_home(); break;
    case PT_K_END: case PT_CTRL('e'): begin_cmd(K_OTHER); cmd_end(); break;
    case PT_K_PGUP: begin_cmd(K_OTHER); cmd_page(-1); break;
    case PT_K_PGDN: case PT_CTRL('v'): begin_cmd(K_OTHER); cmd_page(1); break;

    case PT_CTRL('s'): begin_cmd(K_OTHER); save_buffer(0); break;
    case PT_CTRL('x'): begin_cmd(K_OTHER); do_exit(); break;
    case PT_CTRL('o'): begin_cmd(K_OTHER); cmd_insert_file(); break;
    case PT_CTRL('w'): begin_cmd(K_OTHER); cmd_search(); break;
    case PT_CTRL('n'): case PT_K_F3: begin_cmd(K_OTHER); cmd_search_again(1); break;
    case PT_CTRL('p'): begin_cmd(K_OTHER); cmd_search_again(-1); break;
    case PT_CTRL('r'): begin_cmd(K_OTHER); cmd_replace(); break;
    case PT_CTRL('k'): begin_cmd(K_CUT); cmd_cut(); break;
    case PT_CTRL('u'): begin_cmd(K_OTHER); cmd_paste(); break;
    case PT_CTRL('z'): begin_cmd(K_OTHER); do_undo_cmd(); break;
    case PT_CTRL('y'): begin_cmd(K_OTHER); do_redo_cmd(); break;
    case PT_CTRL('l'): begin_cmd(K_OTHER); cmd_goto_prompt(); break;
    case PT_CTRL('g'): case PT_K_F1: begin_cmd(K_OTHER); E.mark_on = 0; show_help(); break;
    case PT_CTRL('t'): begin_cmd(K_OTHER); toggle_menu(); break;
    case PT_CTRL('c'): begin_cmd(K_OTHER); cmd_where(); break;
    case PT_K_F5: pts_invalidate(&E.scr); break;
    case PT_K_ESC: begin_cmd(K_OTHER); E.mark_on = 0; break;
    default:
        if (k >= 0x20 && k < PT_K_BASE && k != 0x7F) { begin_cmd(K_TYPE); cmd_insert_cp((uint32_t)k); }
        break;
    }
    clamp_cursor();
}

/* ------------------------------------------------------------------ main */

static void parse_plus(const char *s, long *line, long *col) {
    *line = atol(s);
    const char *comma = strchr(s, ',');
    *col = comma ? atol(comma + 1) : 0;
}

static int lang_from_name(const char *s) {
    if (!strcmp(s, "c")) return LANG_C;
    if (!strcmp(s, "sh") || !strcmp(s, "shell") || !strcmp(s, "gapsh")) return LANG_SH;
    if (!strcmp(s, "md") || !strcmp(s, "markdown")) return LANG_MD;
    if (!strcmp(s, "none") || !strcmp(s, "text")) return LANG_NONE;
    return -1;
}

int main(int argc, char **argv) {
    memset(&E, 0, sizeof E);
    E.tabsize = 8;
    E.autoindent = 1;
    E.highlight = 1;
    E.lang_auto = 1;
    E.want_col = -1;
    E.group = 1;
    const char *forced_lang = NULL;
    static const struct option longopts[] = {
        { "linenumbers", 0, NULL, 'n' }, { "nohighlight", 0, NULL, 'S' }, { "noindent", 0, NULL, 'I' },
        { "tabstospaces", 0, NULL, 'E' }, { "tabsize", 1, NULL, 'T' }, { "readonly", 0, NULL, 'R' },
        { "language", 1, NULL, 'l' }, { "version", 0, NULL, 'V' }, { "help", 0, NULL, 'h' }, { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "nSIET:Rl:Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'n': E.linenums = 1; break;
        case 'S': E.highlight = 0; break;
        case 'I': E.autoindent = 0; break;
        case 'E': E.tabs_to_spaces = 1; break;
        case 'T': E.tabsize = atoi(optarg); if (E.tabsize < 1 || E.tabsize > 32) E.tabsize = 8; break;
        case 'R': E.view_only = 1; break;
        case 'l': forced_lang = optarg; break;
        case 'V': printf("pnote %s -- Patchouli's Notebook (TouhouOS)\n", PNOTE_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    const char *file = NULL;
    long goto_line = 0, goto_col = 0;
    for (int i = optind; i < argc; i++) {
        if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) { parse_plus(argv[i] + 1, &goto_line, &goto_col); continue; }
        if (file) { fprintf(stderr, "pnote: only one file at a time (the notebook holds a single page)\n"); return 2; }
        file = argv[i];
    }

    if (file && !strcmp(file, "-")) {
        E.from_stdin = 1;
        if (isatty(0)) { fprintf(stderr, "pnote: nothing to read: standard input is the terminal\n"); return 1; }
        if (buf_load_fd(0, "standard input") < 0) { fprintf(stderr, "pnote: standard input: %s\n", strerror(errno)); return 1; }
        E.filter = !isatty(1);
    } else if (file) {
        E.path = strdup(file);
        struct stat st;
        if (stat(file, &st) < 0) {
            if (errno != ENOENT) { fprintf(stderr, "pnote: %s: %s\n", file, strerror(errno)); return 1; }
            E.new_file = 1;
            buf_reset_empty();
            E.was_empty = 1;
        } else {
            if (S_ISDIR(st.st_mode)) { fprintf(stderr, "pnote: %s: Is a directory\n", file); return 1; }
            int fd = open(file, O_RDONLY);
            if (fd < 0) { fprintf(stderr, "pnote: %s: %s\n", file, strerror(errno)); return 1; }
            int rc = buf_load_fd(fd, file);
            int e = errno;
            close(fd);
            if (rc < 0) { fprintf(stderr, "pnote: %s: %s\n", file, strerror(e)); return 1; }
            if (access(file, W_OK) != 0) E.file_readonly = 1;
        }
    } else {
        buf_reset_empty();
        E.was_empty = 1;
    }

    if (forced_lang) {
        int l = lang_from_name(forced_lang);
        if (l < 0) { fprintf(stderr, "pnote: unknown language '%s' (use c, sh, md or none)\n", forced_lang); return 2; }
        E.lang = l;
        E.lang_auto = 0;
        if (l == LANG_NONE) E.highlight = 0;
    } else {
        E.lang = hl_detect(E.path, E.n ? LN(0)->s : NULL, E.n ? LN(0)->len : 0);
    }

    if (pt_term_open(&E.term) < 0) {
        fprintf(stderr, "pnote: cannot find a terminal to run in (is there a /dev/tty?)\n");
        return 1;
    }
    pt_term_size(&E.term);
    pt_term_enter(&E.term, 1);
    view_layout();
    if (E.new_file) set_msg("New file");
    else if (E.file_readonly) set_msg("%s is read-only: ^S will ask for another name", E.path);
    if (goto_line > 0) cmd_goto(goto_line, goto_col);

    while (!E.quit) {
        if (!pt_pending(&E.term)) view_draw();
        int k = next_key();
        if (k == PT_KEY_EOF) break;
        handle_key(k);
    }

    pt_term_leave(&E.term);
    if (E.filter) {
        if (g_discard) return 1;
        if (buf_write_fd(1) < 0) { fprintf(stderr, "pnote: cannot write the text: %s\n", strerror(errno)); return 1; }
    }
    return 0;
}
