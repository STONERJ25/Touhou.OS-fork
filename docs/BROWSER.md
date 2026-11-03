# Kyouko's browser (`browser`)

The first TouhouOS web browser: a Kappa window around the Hatate engine (`user/lib/web`), libhttp and libtls for the network, libttf for
text (Gensokyo Sans) and libimg for pictures. **Phase 1: pages without JavaScript.** Written on the branch `agent/browser`.

**Why Kyouko.** Kyouko Kasodani is the yamabiko, the mountain echo: she shouts into the valley and an answer comes back, which is what a
browser does with a URL. The pieces around her keep their names: Hatate (the engine) looks at the page through her camera, Kosuzu (the
JavaScript engine) will read its scripts, and Yukari (the network, docs/THEMING.md) opens the gaps the packets travel through. The
start menu says "Kyouko's Browser" (the desktop icon says "Browser", with the torii: the gate between worlds), the window is titled with
the page's title, and the error pages are hers: a plain sentence from the library that found the fault, next to the joke ("Nothing here
but the wind", "That voice is not who it claims to be", "The echo never came back"). The older plan's name, "Bunbunmaru Browser", was
dropped because "Bunbunmaru News" is already an application.

## What it does

`browser [--trace] [URL | PATH]`, or the start menu. A window with a menu bar, a toolbar (back, forward, reload or stop, home, a lock for
verified https pages or a warning triangle for plain http, the address field, Go), a find bar (Ctrl+F), the page, a scroll bar and a
status line (the link under the pointer, progress, notes). Keys and limits are listed on `about:browser` and in `man browser`.

Two fonts are at work and they are separate: the window's chrome (menus, toolbar, find bar, status line, tooltips) is the toolkit's, which since the merge of
`agent/desktop-fonts` is Gensokyo Sans at 13 px, proportional and kerned (docs/TTF.md, "The desktop in Gensokyo Sans"), so the window code **measures**
(`kc_text_width`) where it places things and never counts 8 px cells; the page's text is `fonts.c`'s, through libttf directly at the CSS size. They read the same
file, `/ScarletDevilMansion/share/fonts/GensokyoSans-Regular.ttf`.

| Area | Done |
|------|------|
| Addresses | `http://`, `https://`, `file://` and plain paths (`/x`, `./x`, `~/x`; a folder is shown as an index), `data:`, `about:home`, `about:browser`, `about:blank`; text typed without a scheme becomes `https://` (names with a dot) or `http://` (localhost, addresses, plain names) |
| Network | libhttp on worker threads (4 at once, main page first): redirects (10), chunked and gzip/deflate bodies, the response's charset; the page is parsed while it arrives; `https` through libtls with the certificate checked against `$SSL_CERT_FILE` or `/etc/ssl/cacert.pem`: a refusal is an error page with the library's sentence and **no "continue anyway"** |
| Painting | the whole display list: rectangles and rounded corners, borders (solid, dotted, dashed, double, groove, ridge, inset, outset, joined along the diagonal, rounded), text in Gensokyo Sans with kerning and sub-pixel positions, pictures (copied, enlarged bilinearly, shrunk with an area average), clips, opacity, scroll boxes with scroll bars, shadows, ellipses, and the form controls drawn from their item data; at hi-dpi the page is drawn at the screen's resolution (no enlarging) |
| Interaction | hover and cursor shapes, links, text selection by drag, double click (word) and triple click (paragraph), copy to the Fairies clipboard (1000 bytes: the protocol's limit), find in page, scrolling by wheel, arrows, PageUp/Down, Home/End, Space, scroll bar; scroll boxes scroll under the wheel |
| Forms | text fields and text areas (typing, caret, selection with the keyboard, Ctrl+A/C/X/V, maxlength, password bullets), check boxes, radio buttons, selects (a list drawn over the page), buttons, Tab and Shift+Tab, Enter submits; `method=get` forms navigate to the query URL |
| History | back, forward, reload, stop; the scroll position comes back; fragment links scroll without a fetch; the old page stays on screen until the new one has something to show |
| Pages of its own | the start page, the keys and limits, an empty page, error pages (DNS, refused, timeout, certificate, no root store, cut or garbled answers, redirect loops, too big, missing file, permission, wrong scheme, not an address, an unsupported kind of file, HTTP statuses), the index of a folder |
| Limits (all say so on screen) | a page 8 MiB, a stylesheet or picture 6 MiB, 400 requests per page, 20 s of silence, 90 s per download, 160 MiB of memory for a document, 10 redirects; `file:` is only fetched for pages that are themselves files |

## How the pieces connect

```
 main.c  (Kappa window: menus, toolbar, find bar, scroll bar, status)  ──  core.h API  ──  host callbacks (redraw, clipboard)
   │
 core.c   navigation, history, page lifecycle, rendering          input.c   pointer, wheel, keys, selection, find
 forms.c  text editing, select list, Tab, link/submit actions      errpage.c error pages, about:home, about:browser, about:blank
   │                              │
 loader.c  worker threads         paint.c  display list -> pixels      fonts.c  WebFontProvider on libttf (+ bitmap fallback)
   │  libhttp / libtls / libz            │  libimg
 urls.c  what a person types, file: URLs
```

Nothing in the core includes the toolkit: `tests/host/browsertool.c` runs whole sessions (open, click, type, find, copy, screenshot) on the
host, and the QEMU tests only have to prove the window code and the real machine.

| File | What |
|------|------|
| `user/bin/browser/main.c` | the window: layout, toolbar, find bar, menus, key and pointer translation, the paint trace |
| `user/bin/browser/core.{h,c}` | `BrCore`: the current page and the incoming one, history, loading by content type, subresources for the engine, rendering, the status line |
| `user/bin/browser/input.c` | pointer, wheel, keys, selection (drag, word, paragraph), copy, find |
| `user/bin/browser/forms.c` | field editing (caret, selection, scroll), the select list, Tab order, what a click on a link or button does |
| `user/bin/browser/loader.{h,c}` | `BrLoader`: fetches on threads (http, https, file, data), limits, events, the folder index, the shared root store and resolver lock |
| `user/bin/browser/fonts.{h,c}` | `WebFontProvider` on libttf and the text drawing (kerning, made-up bold and italic, bitmap fallback, hi-dpi) |
| `user/bin/browser/paint.{h,c}` | the rasteriser, and the geometry of form controls |
| `user/bin/browser/urls.{h,c}` | typed addresses, `file:` URLs, display forms |
| `user/bin/browser/errpage.c` | the pages the browser makes itself |
| `initrd/.../patchouli/1/browser.pw` | the manual page |
| `tests/host/browsertool.c`, `tests/host/test_browser.py`, `tests/browser/` | host tests and their fixtures |
| `tests/case_browser.py` | QEMU tests |

### The three engine interfaces (docs/WEB.md)

- **WebFontProvider** (`fonts.c`): `match` picks a face for the font-family list (generic and common names map to Gensokyo Sans; monospace names
  to Gensokyo Mono when `GensokyoMono-*.ttf` exists, else to the 8x16 bitmap font at a whole multiple of its cell, which is what a terminal
  looks like), bold and italic come from `GensokyoSans-Bold/Italic/BoldItalic.ttf` when those files exist and are made up otherwise (a
  glyph drawn twice 0.035 em apart with every advance 0.03 em wider; the outline sheared by 0.2). `measure` is libttf's own text width at the
  CSS size (kerning and ligatures on, advances not rounded: linear in the size, so it equals what is drawn at the device size). A character
  the face has no glyph for is drawn from the toolkit's bitmap font scaled to the text size, and an East Asian wide character takes a whole em;
  the engine and the painter split every run the same way. Files are read from `/ScarletDevilMansion/share/fonts` (`$BROWSER_FONT_DIR` for
  tests). With no font file at all everything is the bitmap font.
- **WebLoader** (`loader.c`, `core.c`): the engine's `fetch` starts a thread-backed download and returns its id; the answer is handed to
  `web_doc_resource_done` on the main thread, never from a worker (the engine wants one thread). Navigation cancels a page's fetches by
  generation. Workers share only the fetch records and chunk queues behind one mutex; the resolver cache in libc has no lock, so the one
  step that touches it (the name lookup, up to libhttp's "connecting to" event) is serialised by the loader; the root store is loaded once
  under a lock by whichever worker needs it first.
- **WebImageDecoder**: `web_image_libimg()`. A picture opened as a page is wrapped in a tiny document and answered from the bytes already in
  hand (no second fetch). The painter keeps area-averaged copies of shrunk pictures in a small cache (24 MiB).

### Where scripts would run

`start_document()` in `core.c` is the one place that builds the `WebConfig`: its `scripts` member is NULL (so `<noscript>` content is shown and
no script is even requested). Kosuzu binds there: a `WebScriptHost` whose `run_script` calls the engine, events from `input.c` dispatched
through wrappers before `web_doc_activate`, timers from the window's tick. Nothing else in the program needs to change.

## Tests

Host (no QEMU; WSL on this PC): `python tools/hosttest.py discover -s tests/host -p "test_browser*.py" -v` (about six minutes with the
sanitized builds; the first run compiles the libraries). 60 tests, in classes:

| Class | What |
|-------|------|
| `Samples` | the engine's sample pages through the real fonts and painter (pictures in `build/browser-pages/`), also at scale 2 |
| `Urls` | 20 typed addresses, `file:` URLs both ways, kinds, display forms |
| `Fonts` | `measure` equals libttf's width (through `ttftool`) at three sizes and four texts; family mapping; monospace cells; made-up bold and italic; metrics; bitmap fallback; painted ink matches the measured width at scales 1 and 2 |
| `Painting` | exact pixels of rectangles, solid borders and their diagonal joins, rounded corners, dashed/dotted gaps, outset shading, opacity, pictures (copied, shrunk, stretched), text colour, hi-dpi, shadows, clipping |
| `Hostile` | absurd pages (billion-pixel sizes, borders, radii, shadows, fonts, tables; a thousand nested groups; 200 KB values) under AddressSanitizer and UBSan at scales 1 to 3, and a document that hits its memory limit |
| `CoreSession` | stylesheet and pictures, links, back/forward, fragments, wheel and keys, scroll restore, find, selection and copy (drag, word, paragraph), the whole form (typing, caret keys, check boxes, radio buttons, select, text area, password, submit URL), Tab, cursors, text and picture pages, error pages, data: URLs, resizing |
| `Loader` | local HTTP servers: a page with a stylesheet and a picture, redirects and a loop, chunked and gzip bodies, a page shown while a slow body arrives, text/picture/other types, 404 with and without a body, garbage, a body cut short, a silent server (timeout), a refused port, the page size limit, ISO-8859-1, Stop |
| `Https` | a TLS server with test certificates (made with `cryptography`): verified page with its subresources; unknown authority and wrong name refused with the library's sentence; no root store |

QEMU: `python tools/qemu-slot.py -- python tests/run_all.py browser_` (about two minutes): `browser_http_pages` (a page with a picture, a link
clicked, back, Page Down to the bottom, an address typed, find in page, a form submitted), `browser_files_text_and_pictures`, `browser_start_page_and_menus` (the start page; the Help menu opens over the toolbar and leaves nothing behind),
`browser_window_resize_and_paint_speed` (maximise; paint statistics from the trace), `browser_hidpi_paints_natively` (2560x1440: the text has
edges at every screen pixel), `browser_https_and_the_certificate_check` (a CA fetched into the guest; refused without one), `browser_internet` (optional, see "Measured").
`python tools/hosttest.py tests.host.test_browser.Scratch` runs the session script in `build/browser-scratch.txt` (commands: the header of
`tests/host/browsertool.c`), the quickest way to look at a page.

### Measured (2026-11-03, QEMU with TCG on the development PC; the Dell has not run it)

- Paint of a page into the window surface: 4 to 23 ms on average, 59 ms the worst frame, for the engine's article page and the test pages
  (the whole viewport is repainted when anything changes; there was no need for partial repaints). The browser logs this with `--trace`.
- `browser` is 1.07 MB stripped (the engine is most of it); the initrd grew from 15.5 to 16.1 MiB (the build prints 16.5 MiB with the kernel; the
  push limit is about 21 MiB).
- A whole session through HTTPS in QEMU (boot to loaded page): the browser QEMU groups take 15 to 25 s each.
- Real pages (`browser_internet`, optional: needs internet and the `certifi` package; QEMU's user networking reaches the real internet): example.com loads over TLS 1.3 with the
  lock; `expired.badssl.com` is refused with "the certificate ... expired on 2015-04-12 23:59:59 UTC; the clock says ...". www.gnu.org renders properly (logo, menu bar,
  headings, links). en.wikipedia.org loads (title, header, contents box, scroll bar) but its layout is rough (flex, grid, sticky and script-dependent parts of the engine; the first
  paint takes about a second in QEMU, almost all of it the engine's layout of a large page): what the engine lacks is in docs/WEB.md.

## Limits and known gaps

- **No JavaScript** (see above), so most modern sites are empty shells. No `<iframe>`, `<video>`, `<canvas>`, SVG, `@font-face`, CSS transforms,
  animations or transitions (the engine's list, docs/WEB.md); GIFs show their first frame; WebP and SVG pictures show a broken-picture box.
- **POST** is not supported (libhttp makes GET requests only): such a form leaves a note in the status line. No cookies (the jar is not
  thread-safe and nothing needed it yet), no HTTP authentication, no proxy, no downloads to disk (a file the browser cannot show is refused
  before it is read: "Kyouko can only echo pages").
- One window, one page, no tabs; no bookmarks; no zoom (the engine has a device scale, not a CSS zoom); no print; no "view source".
- The root store is `/etc/ssl/cacert.pem`. Since `b0bb254` on `main` the image build puts the owner's bundle there from `assets/local/ssl/cacert.pem`
  (git-ignored), so https works after every start of an image built where that file exists; otherwise every https page is the "no list of trusted voices"
  error page (and `SSL_CERT_FILE` can name a bundle elsewhere).
- Opacity is approximated (every item inside a group is drawn with the group's alpha, so overlapping children show through each other);
  a rounded clip clips to its rectangle (a rounded avatar is a square); text selection inside fields is by keyboard only; a text area has no
  mouse selection; the select list is not scrollable by wheel; the clipboard carries 1000 bytes (the Fairies protocol).
- Characters the font lacks (accents, ©, euro, all of Japanese) come from the 8x16 bitmap font or show a box until the fonts get wider coverage
  (the font specification); a CJK fallback face would plug into `fonts.c` as one more slot.
- A decoded picture is decoded in the window's thread the first time it is painted (a 1920x1080 JPEG takes 0.4 to 0.7 s in QEMU).
- `https` to `http` redirects are followed (libhttp warns in the log only); mixed content (http images on https pages) is allowed.

## Changes outside the browser's own files

- `user/bin/hakurei-shell/launch.c`: one line (the start menu entry "Kyouko's Browser", icon KI_TORII). `desktop.c`: one entry in the desktop icon list ("Browser").
- `user/lib/web/src/layout.h`: `to_unit()` clamps lengths to +-2^28 layout units (4.2 million CSS pixels). The hostile page test found a signed overflow
  in layout (`layout.c:705`) from `left:-1000000000px`; the engine's own suite (`test_web.py`) still passes (18 tests, 2 skipped for the optional html5lib/WPT downloads).
  The regression test is `Hostile` in `tests/host/test_browser.py`.
- `user/lib/http/include/http.h`, `src/parse.c`: a header field may now be as long as a line (8 KiB; it was 4 KiB). Loading the real Wikipedia page failed with
  "a header field longer than 4 KiB": its Content-Security-Policy is 4.5 KiB. libhttp's own host tests pass with the change (12 of 12 in `HttpParsing`,
  `HttpUrls`, `Cookies`, `HttpClient`); the regression test is `Loader.test_long_header_fields_as_real_servers_send_them` (6000 bytes load, 9000 stay refused). docs/TLS.md names the new limit.
  *Not mine, found on the way:* `tests/host/test_tls_vectors.py` no longer builds on `main` (its source list globs all of `user/lib/sshcrypto/src`, and the ssh client's
  `sshc_*.c` files include the OS's `crypto.h` and `gensokyo.h`); restricting the list to `curve25519.c`, `sha512.c`, `chachapoly.c` (all libtls needs) fixes it, as in `tests/host/test_browser.py`.
- `initrd/ScarletDevilMansion/share/patchouli/1/browser.pw`: the manual page (the `manual` group passes with it).
- `docs/STATUS.md`: a Browser row, and the Web engine row no longer says there is no browser.

## State when paused / how to resume

State on 2026-11-03: everything above is committed on `agent/browser` (not pushed, as the rules say) and passes: the host tests (60), the engine's suite, and
the QEMU groups `browser_` (6 tests) and `manual`. Nothing has run on the Dell. The merge into `main` needs only a `git merge`: the shell edits are
two lines, everything else is new files.

**Next steps, most valuable first:**
1. **Try it on the Dell** after the merge: `browser https://example.org/` needs `/etc/ssl/cacert.pem` and a clock that `sntp` has set (docs/TLS.md); look at
   `browser --trace` output for the paint times, and at the hi-dpi scale on the owner's monitor (checked only in QEMU).
2. **JavaScript**: bind Kosuzu (docs/JS.md) through a `WebScriptHost` in `start_document()`; the engine already pauses the parser for scripts and reports mutations.
   The missing RegExp and Date in Kosuzu limit what real pages can do.
3. **POST and cookies**: libhttp needs a request body and a thread-safe cookie jar (or a lock around it); forms that post and every login would follow.
4. Smaller: tabs, a bookmarks page, zoom (a CSS zoom in the engine), the `cacert.pem` persistence, a CJK fallback face, rounded clips, mouse selection in text areas,
   saving what the browser cannot show to `~/Downloads`, animated GIFs through `img_anim_next`, `<meta http-equiv=refresh>`.

**Things worth knowing when changing it.** Tests that start the browser need `text` on the kernel command line and `net=True`; the first window's content is
at (44, 56) and the page starts 58 pixels below (menu 24, toolbar 34). The menu titles are narrower in Gensokyo Sans than in the bitmap font (Help starts at x = 200
instead of 227), so the tests click inside the title in both layouts (x = 236). A click in a QEMU test can miss when a PS/2 mouse packet is lost on a busy PC:
the steps that depend on one aim again (`click_until` in `tests/case_browser.py`). `BROWSER_TIMEOUT_MS`, `BROWSER_MAX_DOC_BYTES` and `BROWSER_FONT_DIR` are knobs for
tests only. The engine's display list is valid only until the next layout or paint call: the core never keeps one (`page_control_geom` paints the control's
small rectangle when it needs a control's geometry). In a Git Bash heredoc a backslash in Python source is eaten: write test code with the editor tools.

## Seen on the Dell (2026-11-03, build `8c43292`)

Driven through VNC (`tools/vncshot.py`): a terminal opened with Ctrl+Alt+T, `browser https://example.com/`, then the address field (Ctrl+L) for `https://www.wikipedia.org/` and `https://expired.badssl.com/`, at 2560x1440 (ui scale 2, the page drawn natively).
example.com and the Wikipedia portal load over TLS with the certificate checked against the shipped bundle (lock icon in the address bar; the portal shows its globe, the language list with the article counts and the search box). What is wrong there: the Chinese, Japanese, Russian, Arabic and Hindi language names are boxes (the face has no glyphs
for them), and the accented Latin letters (ç, ñ, ê, í) come out as small bitmap cells beside the proportional text (the face has no Latin-1 composites yet): both are font work (docs/TTF.md), not the browser's. The expired certificate is refused with the in-theme page ("That voice is not who it claims to be"), the library's exact sentence and no way to continue.
After the three pages the Dell was idle again (35 C, 3.5 W). Not tried on the Dell: forms, find, long pages, downloads of large pages, the Help menu.
