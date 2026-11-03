# Hatate: the web engine core (user/lib/web)

Hatate turns HTML and CSS into a display list. It is a pure C library with no operating-system dependency: fonts,
networking, image decoding and scripts come in through small interfaces (`user/lib/web/include/web.h`) that other
parts of TouhouOS implement. It is written from the specifications (WHATWG HTML, CSS 2.1, CSS Syntax 3, Selectors 4,
CSS Text 3, Flexbox 1, Grid 1, Media Queries 4); no code was taken from other engines.

```
bytes ──encoding──▶ tokenizer ──▶ tree builder ──▶ DOM ──cascade──▶ computed styles
      ──box tree──▶ layout (block, inline, floats, positioning, flex, grid, tables) ──▶ display list, hit tests
```

## Using it

```c
WebConfig cfg = { .fonts = &my_fonts, .loader = &my_loader, .images = web_image_libimg() };
WebDoc *doc = web_doc_new(&cfg, "https://example.org/");
web_doc_parse(doc, bytes, len, NULL);                  /* or web_doc_parse_bytes per piece + web_doc_parse_finish */
web_doc_layout(doc, 800, 600);                          /* viewport in CSS px; cheap when nothing changed */
const WebDisplayList *dl = web_doc_paint(doc, 0, scroll_y, 800, 600);
WebHit hit;
if (web_doc_hit_test(doc, x, y + scroll_y, &hit) && hit.href) navigate(hit.href);
web_doc_free(doc);
```

Everything is in `web.h`, which documents each call. The groups:

| Area | Calls |
|---|---|
| Documents | `web_doc_new/free/status`, `web_doc_parse`, `_parse_bytes`, `_parse_finish`, `_parse_resume`, `web_doc_write`, parse errors |
| DOM | node accessors, text, attributes, create/append/insert/remove/replace/clone, `inner_html`/`outer_html`/`set_inner_html`, `get_element_by_id`, by tag/class, `web_query_selector(_all)`, `matches`, `closest` |
| Styles | `web_element_style_get/set` (style attribute), `web_element_computed` (getComputedStyle), hover/active/focus/target states |
| Layout | `web_doc_layout`, `web_doc_content_size`, `web_element_rect`, page and element scrolling, `web_doc_scroller_at`, `web_doc_fragment_position` |
| Painting | `web_doc_paint` → `WebDisplayList` of `WebDisplayItem` (rect, border, text, image, clip push/pop, opacity push/pop, scroll push/pop, control, ellipse, shadow) |
| Interaction | `web_doc_hit_test` (node, caret offset, element, link + resolved href, form control, cursor), selection, `web_doc_selection_text`, `web_doc_find`, `web_doc_activate` (links, submit, checkboxes, labels, details), control values |
| Debugging | `web_dump_dom` (html5lib format), `web_dump_layout` |

Coordinates: layout works in `WebUnit` = 1/64 CSS px. Display lists are in device pixels (CSS px × `WebConfig.scale`)
relative to the requested rectangle; text origins in 1/64 device px. `position: fixed` boxes are drawn relative to
the rectangle's corner (the viewport).

## Interfaces other parts implement

- **WebFontProvider** (`match`, `metrics`, `measure`): the engine asks once per distinct font description and keeps the
  handle; text runs in the display list carry the same handle, so the painter draws what was measured. Without one a
  built-in fixed-width font is used (half the font size per character, full size for East Asian wide characters,
  ascent 3/4): at 16 px it measures like the 8x16 bitmap font, which is what the tests use.
- **WebLoader** (`fetch`, `cancel`): stylesheets (`<link>`, `@import`), images, scripts (only with a script host). The
  engine resolves URLs (document URL, `<base href>`) and asks for absolute ones; the embedder answers with
  `web_doc_resource_done`, possibly from inside `fetch`.
- **WebImageDecoder** (`probe`, `decode`, `release`): `probe` gives the intrinsic size when bytes arrive (layout needs
  it), `decode` runs the first time an image is painted. `web_image_libimg()` (imgadapt.c) is the decoder on
  `user/lib/img`; its handles are `const Image *`.
- **WebScriptHost** — the DOM bridge a JavaScript engine binds to: `run_script` for parser-inserted scripts (return 1
  to pause the parser, resume with `web_doc_parse_resume`; `web_doc_write` is document.write), `mutation` records
  after every API or late-parser change (MutationObserver), `lifecycle` (DOMContentLoaded, load), `document_freed`.
  `web_node_user/set_user` holds one pointer per node for the wrapper object. Events are the browser's: it hit-tests,
  dispatches DOM events through its own wrappers, and calls `web_doc_activate` unless a handler prevented the default.

## What is supported

**HTML parsing**: the whole WHATWG tokenizer (all states, the full named character reference table) and tree builder
(every insertion mode including tables, select, template, foreign content; foster parenting, the adoption agency,
the 2025 changes such as customizable `<select>`, `<selectedcontent>` cloning). Encodings: BOM, transport charset,
the `<meta>` prescan, UTF-8, windows-1252 (and the WHATWG labels that map to it), Shift_JIS and EUC-JP (tables generated
from the WHATWG indexes), UTF-16. Conformance: html5lib tokenizer tests 7049/7049 token streams (7046/7049 error lists:
the three are an outdated error name in one file), WPT tree-construction 1967/1967 (the `scripted_*` files need a
script engine and are skipped). Parse errors are recorded with WHATWG codes and positions.

**CSS**: Syntax 3 tokenizer and parser; `@media` (Media Queries 4 with ranges, nested), `@import` (through the loader,
depth-limited), `@supports`; selectors 3 plus `:is/:not/:where/:has` (relative selectors), attribute selectors with
`i`/`s`, `::before/::after/::marker/::placeholder`; specificity; the cascade with origins, `!important`, inheritance,
`initial/inherit/unset/revert`; custom properties and `var()` with fallbacks and cycle detection; `calc()`, `min()`,
`max()`, `clamp()`; units px, em, rem, ex, ch, vw, vh, vmin, vmax, %; named, hex, rgb(a), hsl(a) colours; the common
properties (box model, fonts, text, backgrounds, borders, outlines, lists, positioning, flex, grid, tables, overflow,
opacity, box-shadow, cursor, visibility...). Presentational attributes (bgcolor, width, align, font, table attributes)
map to styles. A UA stylesheet of its own (uasheet.c).

**Layout**: block formatting with margin collapsing (siblings, parent and child, empty blocks, negative margins),
`box-sizing`, min/max sizes, auto margins, percentages; floats with clearance, line boxes and formatting contexts beside
them; relative, absolute and fixed positioning with static positions; sticky laid out as relative; inline formatting
with white space processing (normal, pre, nowrap, pre-wrap, pre-line, break-spaces), text-transform, letter- and
word-spacing, text-indent, text-align (justify too), vertical-align (all keywords and lengths), line-height, break
opportunities (spaces, CJK with basic kinsoku, hyphens, U+200B, soft hyphens, `<wbr>`, around atomic inlines),
word-break and overflow-wrap; inline-block, inline-flex/grid/table; replaced elements (intrinsic sizes and ratios,
aspect-ratio, object-fit when painting) and form controls as boxes; list markers (outside and inside, counters,
ol start/reversed, li value, counter-reset/-increment/-set, generated content with counters and quotes); flexbox
(direction, wrap, order, grow/shrink/basis with the freezing loop, automatic minimum size, gaps, justify-content,
align-items/-self/-content, auto margins); grid (explicit and implicit tracks, fr, minmax, fit-content, repeat with
auto-fill/auto-fit, areas, line numbers and names, spans, auto-placement row/column/dense, gaps, self alignment);
tables (anonymous table objects, auto and fixed layout, colspan/rowspan, `<col>` widths, percentages, border-spacing,
the collapsing border model approximated, captions, header/footer groups, vertical-align in cells); overflow clipping
and scroll containers with their scroll offsets.

**Painting**: CSS 2.1 Appendix E order (stacking contexts by z-index, block backgrounds, floats, inline content,
positioned boxes), backgrounds (colour, one image with size/position/repeat, linear gradients approximated by bands),
borders with per-side widths/colours/styles/radii, outlines, box-shadow (outer), text decorations, selection
highlights, list bullets, images, controls (the UI draws them from kind, state and text), clips, opacity groups.
`tests/host/webtool.c render` is a reference rasteriser that writes PPM files.

## What is missing or approximated

- Not implemented: `@font-face`, `@layer` ordering, `@container`, `::first-line`/`::first-letter`, CSS transforms,
  transitions and animations, multi-column layout, `writing-mode` and bidi reordering (`direction: rtl` only flips
  text-align), `text-overflow: ellipsis`, `hyphens: auto`, `shape-outside`, filters, `clip-path`, masks,
  `position: sticky` sticking, subgrid, masonry, baseline alignment in grid, column backgrounds of tables, inset
  shadows, more than one background layer or shadow, `<iframe>`/`<video>`/`<canvas>`/SVG content (their boxes are laid
  out empty), declarative shadow DOM, custom elements, ISO-2022-JP and the other legacy encodings, IDNA.
- Approximations: the CSS 2.1 block-in-inline split is not done (an inline containing blocks becomes a block); the
  collapsing border model gives each grid line one width; grid track sizing spreads spanning items' space evenly;
  absolutely positioned boxes inside a relatively positioned inline use the next block ancestor; positioned boxes with
  z-index auto are painted as if they made a stacking context; percentage heights resolve only against definite
  heights; the line probe for floats uses the strut's height.
- Measured: a 1 MB generated document (paragraphs, tables, flex rows) parses in 38 ms, styles in 118 ms, lays out in
  189 ms and paints the first screen in 31 ms (-O2 on the development PC, WSL), with 122 000 boxes. Memory: 265 MB of
  budget for that page, which is too much (layout re-runs for flex and table measuring leave garbage in the layout
  arena, and computed styles are large); the per-document limit (`WebConfig.memory_limit`) holds and layout then
  stops gracefully (`web_doc_status` returns `WEB_E_NOMEM`).

## Memory model

A document owns all its memory through `WebBudget` (the embedder's allocator, or malloc) and a few arenas: the DOM and
stylesheets (`arena`), computed styles (two arenas used in turn so a full restyle frees the old styles at once), the box
tree (`larena`, reset by every layout), display lists (`parena`, reset by every paint). Arenas allocate from 64 KiB
chunks with free lists per size class (16 to 4096 bytes) so memory freed while the page lives is reused; big blocks
are allocated and freed one by one. `web_doc_free` releases a page at once. Strings and arrays the API returns are
plain allocations freed with `web_free`. Parsing caps the tree depth (`WebConfig.max_depth`, 512 by default); layout
caps its recursion and gives zero-size boxes beyond it.

## Files

`src/`: `mem.c` (arenas), `atoms.c`, `tokenizer.c`, `entities.c` (generated by `gen/genentities.py`), `tree.c`,
`encoding.c`, `jistables.c` (generated by `gen/genjis.py`), `dom.c`, `parser.c`, `url.c`, `resource.c`, `dump.c`,
`csstok.c`, `cssval.c`, `css.c`, `selector.c`, `style.c`, `uasheet.c`, `fonts.c`, `forms.c`, `boxes.c`, `layout.c`,
`inline.c`, `flex.c`, `grid.c`, `table.c`, `image.c`, `imgadapt.c`, `paint.c`, `interact.c`, `ldump.c`.

## Tests

`python tools/hosttest.py discover -s tests/host -p "test_web.py"` builds `tests/host/webtool.c` with the library under
AddressSanitizer and UBSan and runs: the repository's tokenizer and tree-construction cases (and the html5lib and WPT
suites when fetched into `build/web-fetch/`, never committed: `html5lib-tests/tokenizer/*.test` from
github.com/html5lib/html5lib-tests and `wpt/html/syntax/parsing/resources/*.dat` from web-platform-tests), selector
specificity and matching tables, cascade tables, about 80 layout cases with hand-computed boxes, lines and fragments
(`tests/web/layout/`), hit testing, find, painting order, fuzzing of random hostile pages through every stage, deep
nesting and the memory limit, the 1 MB speed measurement, and the sample pages of `tests/web/pages/` (an article with
a sidebar, tables, a form, flex cards and a grid) rendered to `build/web-pages/*.ppm`.

## State when paused / how to resume

Paused on 2026-11-03 (branch `agent/web-core`) when the owner's budget ran out.

- **Solid**: parsing (conformance numbers above), the DOM API, selectors, the cascade and computed values, block and
  inline layout, floats, positioning, the flex algorithm, tables in the separated model, painting order, hit testing.
  Every test in `test_web.py` passes.
- **Partial**: grid track sizing (simplified), the collapsing border model, memory use of big pages, painting of
  controls (the embedder draws them from the item), images (decode path tested through the sample pages' loader only
  lightly), selection text across blocks, `web_dump_layout` format (debugging only).
- **Next steps**: (1) cut layout memory: free the frag/line arrays of measuring passes and cache intrinsic sizes per
  pass so flex/table re-layouts do not repeat work; (2) the block-in-inline split and `::first-line`/`::first-letter`,
  which real pages notice; (3) wire the WebFontProvider to the TrueType rasteriser and the display list to the
  browser window, then compare the sample pages against a reference browser by eye.
