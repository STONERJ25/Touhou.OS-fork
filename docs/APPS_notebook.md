## Patchouli's Notebook (notebook)

The text editor of the desktop. Patchouli never forgets a page, so neither does the Notebook: every edit can be undone, a page
with unsaved changes carries a `*` in its title, and closing such a page always asks first.

```
notebook [file]          open a file; a path that does not exist yet starts an empty page with that name (Save creates it)
notebook --selftest      run the document model tests on the console (no window server needed)
```

Relative paths are fine (`notebook ../notes.txt`), so are spaces (the Explorer starts the editor with the path as one
argument). Start menu: **Patchouli's Notebook**. Window title: `Notebook - report.txt`, `Notebook - *report.txt` when modified,
`(read-only)` appended for files you may not write.

### What it does

* **Editing**: multi-level undo and redo (typing, backspacing and deleting merge into word-sized steps; Replace All, indent and
  paste are one step each; undoing back to the saved text clears the `*`), cut/copy/paste, select all, auto-indent (Enter keeps
  the indentation of the line; in C a `{` adds one level and `{|}` opens a block), Tab / Shift+Tab indent and unindent the
  selected lines, duplicate / delete / move lines, word-wise movement and deletion, smart Home.
* **View**: line-number gutter, current-line highlight, word wrap (wraps at blanks, long words are cut), tab width 2 / 4 / 8,
  a switch for the status bar, syntax colours for C, shell and Markdown (switchable, language can be forced from the View menu
  or by clicking the language in the status bar). Tab inserts spaces or real tabs (View menu; a file that is already indented
  with tabs switches itself to tabs).
* **Find and replace bar** (Ctrl+F / Ctrl+H): live search while you type, Prev / Next (Shift+F3 / F3, Enter / Shift+Enter in the
  field), "Match case", all matches in view are highlighted, the number of matches is shown, Replace and Replace All (one undo
  step). Searching wraps around the end of the page.
* **Go to line** (Ctrl+G): `120` or `120:5`.
* **Status bar**: line and column, number of selected bytes, a message area, line count, size of the file as it will be saved,
  encoding (`UTF-8`, `UTF-8?` when the file has bytes that are not valid UTF-8), line endings (`LF`, `CRLF`, `CR`; a `*` marks a
  file whose endings were mixed), language. Click the line endings to switch LF / CRLF (the page counts as modified), click
  the language to cycle through the colour schemes.
* **Files**: UTF-8 text with LF, CRLF or classic Mac CR line endings (kept on save; mixed files are normalised to their majority
  and the status bar says so), UTF-8 BOM kept, invalid UTF-8 bytes shown as boxes and written back unchanged, files up to 64 MB
  (a 100,000 line file scrolls without a hitch). Files with zero bytes are refused politely ("not a text file"), folders and
  devices are refused, read-only files open read-only (edits are blocked with a message; Save offers Save As), unreadable
  files give an error box. Saving over a different existing file asks "Replace file?", saving a file that changed on disk
  since it was opened asks "Overwrite?".
* **Recent files** (File menu, newest first, ten entries) and the View preferences are remembered between runs.
* **Close, Quit and the window's close button**: with unsaved changes the Notebook asks **Save / Don't save / Cancel** (Esc =
  Cancel). The stock `k_confirm_dialog` has only two buttons and cannot tell Esc from "No", so this one is a small dialog of the
  Notebook's own; `k_confirm_dialog` is used for the overwrite questions.
* Right click in the text opens a context menu (Undo, Redo, Cut, Copy, Paste, Select All).

### Keyboard shortcuts

| Keys | Action |
|------|--------|
| Ctrl+N / Ctrl+Shift+N | new page / new window |
| Ctrl+O | open (file dialog) |
| Ctrl+S / Ctrl+Shift+S | save / save as |
| F5 | reload the file from disk (asks first when there are unsaved changes) |
| Ctrl+W | close the page (the window stays, empty) |
| Ctrl+Q | quit |
| Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z | undo / redo |
| Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+A | cut, copy, paste, select all (Shift+Delete, Ctrl+Insert, Shift+Insert also work) |
| Ctrl+F, Ctrl+H | find, find and replace |
| F3 / Shift+F3 | next / previous match |
| Ctrl+G | go to line |
| Tab / Shift+Tab, Ctrl+] / Ctrl+[ | indent / unindent (the selected lines, or the current line) |
| Ctrl+Shift+D / Ctrl+Shift+K | duplicate / delete line |
| Alt+Up / Alt+Down | move the line(s) up / down |
| Ctrl+Left / Right | word left / right (add Shift to select) |
| Ctrl+Backspace / Ctrl+Delete | delete word before / after the caret |
| Home / End | start (first non-blank, then column 1) / end of the line; with word wrap: of the row |
| Ctrl+Home / Ctrl+End, PageUp / PageDown | start / end of the page, one screen |
| Alt+F, Alt+E, Alt+V, Alt+H, F10 | open the File, Edit, View or Help menu (arrows, Home/End, Enter and Esc work inside; Left/Right switch menus) |
| F1 | list of shortcuts |
| Esc | close the find bar (and dialogs) |

Mouse: click places the caret, drag selects (the view scrolls when you drag past the edge), double click selects a word, triple
click a line, a click on a line number selects that line, Shift+click extends the selection, the wheel scrolls three rows per
notch (Shift+wheel scrolls sideways), the scroll bars work as usual.

### Files the Notebook writes

| File | Contents |
|------|----------|
| `~/.config/notebook.recent` | recently used files, one absolute path per line, newest first, at most 10 lines |
| `~/.config/notebook.conf` | `key=value` lines: `line_numbers`, `word_wrap`, `tab_width` (2, 4, 8), `status_bar`, `highlight`, `indent_spaces` (0 or 1) |

`~/.config` is created when needed. Documents are written in place (`open(O_TRUNC)`), with CRLF / CR conversion and the BOM
applied on the way out.

### Notes for developers

* Source: `user/bin/notebook/` (`doc.c` gap buffer, line index and undo; `editor.c` view, wrap, input; `syntax.c`; `find.c`;
  `fileio.c`; `dialogs.c`; `main.c` menus, status bar and file flows; `selftest.c`). Only the lines on screen are ever laid
  out, so file size does not matter for drawing; the highlighter keeps one small state per line and rescans lazily.
* The system clipboard of the window server holds 1 KiB. The Notebook keeps the full copied text itself and passes the first
  KiB on to the clipboard; pasting uses its own text as long as the system clipboard still holds that same KiB, otherwise
  whatever another program copied.
* `NOTEBOOK_TRACE=1` makes it print `nb: ...` lines on stdout (position, title, saves, dialogs); the tests wait for them.
* Tests: `python tests/run_all.py notebook` (`tests/case_notebook.py`): the model self test, typing and saving byte-exact,
  find and replace, CRLF / BOM / invalid UTF-8, the unsaved-changes dialog, overwrite questions, a 100,000 line file,
  binary / folder / read-only / empty / 1 MB single line files, word wrap and mouse selection, menus and recent files,
  preferences and syntax colours (pixel checks), context menu, paths and New Window.
* Known limits: no multiple documents per window (use New Window), no regular expressions in Find, no drag and drop of
  text, characters are one cell wide (the 8x16 font has no wide glyphs and no combining marks), a drop-down menu that does not
  fit into a very small window is cut off at the window edge.
