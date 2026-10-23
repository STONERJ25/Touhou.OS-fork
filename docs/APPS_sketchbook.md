## Marisa's Sketchbook (sketchbook)

A small paint program for the HakureiDE desktop. It draws on a canvas of up to 4096 x 4096 pixels with a pencil, a round
brush, an eraser, a spray can, lines, rectangles, ellipses, flood fill, a colour picker, text in the built-in 8x16 font and
rectangular selections that can be moved, copied and pasted. It zooms from 100% to 800% with a pixel grid, has 200 levels
of undo, and opens and saves 24-bit BMP and binary PPM (P6) pictures.

Start it from the panel ("Gensokyo" menu, Programs, Marisa's Sketchbook) or with `sketchbook [file]`. The path may be
relative or absolute and may contain spaces (the Explorer starts it through `execv`, so the path arrives as one argument).
A path that does not exist yet opens an empty picture with that name, so Save creates the file. A file that is not a valid
BMP or PPM shows an error dialog and an empty canvas. The window title is `Sketchbook - name`, with ` *` appended while
there are unsaved changes.

```
 File  Edit  Image  View  Help
 +-----+ +----------------------------------------------------+
 | tools| |                                                    |
 | ...  | |                canvas (scrolls, zooms)             |
 |options| |                                                   |
 +-----+ +----------------------------------------------------+
  fg/bg  16 colours   R/G/B sliders   #hex   Editing the foreground
  hint .........  x, y   colour under the pointer   size   [-] 100% [+]
```

### Tools

| Key | Tool | What it does |
|-----|------|--------------|
| P | Pencil | Single pixels; drag to draw lines of pixels. |
| B | Brush | Round brush, width 1 to 32. |
| E | Eraser | Square eraser, width 1 to 32. It paints the background colour with either mouse button. |
| A | Spray can | Scatters random dots inside a circle (radius 1 to 32) while the button is held, even without moving. |
| L | Line | Drag from one end to the other, width 1 to 32. Shift snaps to horizontal, vertical or 45 degrees. |
| R | Rectangle | Drag corner to corner. Styles in the tool options: Outline, Solid (filled with the drawing colour), Both (outline in the drawing colour, inside in the other colour). Width 1 to 32 grows inwards. Shift makes a square. |
| O | Ellipse | The ellipse that fits the dragged box, same styles and width. Shift makes a circle. |
| F | Fill | Flood fill (4-connected, exact colour match). Iterative, so a 4096 x 4096 picture is no problem. |
| I | Colour picker | Click a pixel to take its colour (left: foreground, right: background). |
| T | Text | Click where the text goes, type. Enter places it, Shift+Enter starts a new line, Esc throws it away. Clicking inside the text box moves the caret, clicking elsewhere places the text and starts a new one. Options: scale 1x to 4x, "Opaque" (paint the background colour behind the letters). The text is drawn pixel by pixel with the 8x16 bitmap font. |
| S | Select | Drag a box. Drag inside it to move the pixels (the old place is filled with the background colour); Ctrl+drag moves a copy. Arrow keys nudge a selection (Shift: 8 pixels). Delete clears it, Esc cancels a move or deselects. |

The left mouse button draws with the foreground colour and the right button with the background colour (for rectangles and
ellipses the two colours swap roles). `[` and `]` make the brush, eraser, spray, line and shape width smaller and larger.
Shapes and lines are previewed while dragging; they are painted when the button is released. Esc during a drag cancels it.

### Colours

Sixteen colours in the PC-98 manner (four-bit levels: a dark row and a bright row). Left-click a swatch for the foreground
colour, right-click for the background colour. The two large swatches at the left show the current colours; clicking one
chooses which of them the mixer edits (it gets a red frame). The mixer has R, G and B sliders and a hex field (`#RRGGBB`,
the `#` is optional; the colour changes as soon as six hex digits are valid, Enter returns to the canvas). `X` swaps the
foreground and background colours, as does the small arrows button.

### Zoom, grid and scrolling

* Zoom 1 to 8 with the keys `1`..`8`, `+` and `-`, Ctrl+wheel (around the pointer), the `[-] 100% [+]` buttons in the
  status bar or the View menu. The zoom keeps the point under the middle of the view (or the pointer) in place.
* The pixel grid shows from 400% on; `G` hides or shows it.
* Scroll with the mouse wheel (Shift+wheel sideways), the scroll bars, the arrow keys, Page Up/Down, or pan by dragging
  with the middle mouse button.
* The status bar shows the picture position under the pointer, the colour there, the picture size and the zoom.

### Selections and the clipboard

Cut, Copy and Paste use a clipboard inside the program (the window system's clipboard holds only 1 KiB of text). It
survives New and Open, so pictures can be assembled from parts of other pictures within one running program. Paste puts the
copy at the top left of the visible area as a floating selection that can be dragged; clicking outside places it. Undo
while something is floating cancels the floating selection instead. Image > Crop to Selection makes the selection the
whole picture.

### Menus

| Menu | Items |
|------|-------|
| File | New... (size dialog with presets, 1 to 4096 pixels; larger values are clamped after a note), Open..., Open Screenshot (loads `/tmp/screenshot.ppm`, which Ctrl+F12 writes), Save, Save As..., Quit |
| Edit | Undo, Redo (the item names the operation), Cut, Copy, Paste, Clear Selection, Select All, Deselect |
| Image | Flip Horizontal, Flip Vertical, Rotate 90 Clockwise, Rotate 90 Anticlockwise, Invert Colours, Resize Canvas... (new width and height, optionally stretching the picture; new space is filled with the background colour), Crop to Selection, Clear Image |
| View | Zoom In, Zoom Out, 100%, 200%, 400%, 800%, Show/Hide Pixel Grid |
| Help | Keyboard Shortcuts, About |

### Keyboard shortcuts

| Keys | Action |
|------|--------|
| Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+Q (Ctrl+W) | New, Open, Save, Save As, Quit |
| Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z) | Undo, Redo |
| Ctrl+X, Ctrl+C, Ctrl+V | Cut, Copy, Paste |
| Ctrl+A, Ctrl+D, Delete | Select All, Deselect, Clear Selection |
| P B E A L R O F I T S | Tools |
| `[` `]` | Smaller, larger width |
| X | Swap foreground and background |
| `1`..`8`, `+`, `-`, Ctrl+`=`, Ctrl+`-`, Ctrl+`0` | Zoom |
| G | Pixel grid |
| F1 | Keyboard shortcuts |
| Esc | Cancel a drag, a floating selection or unfinished text; otherwise deselect |
| Shift (while dragging) | Straight lines, squares, circles |
| Ctrl (when starting a selection move) | Move a copy |

Closing the window, Quit, New and Open ask first when there are unsaved changes (Save, Discard, Cancel). Save asks for a
name when the picture has none, and Save As asks before replacing an existing file. A name without an extension gets
`.bmp`; names ending in anything but `.bmp` or `.ppm` are refused with an explanation.

### File formats

* **BMP**: written as 24-bit, uncompressed, bottom-up with the usual 54-byte header. Read: BITMAPCOREHEADER and the 40, 52,
  56, 108 and 124 byte info headers; 1, 4 and 8 bit palettes; 16, 24 and 32 bit pixels; BI_RGB and BI_BITFIELDS; bottom-up
  and top-down. RLE-compressed files are refused with a message.
* **PPM**: written as binary `P6` with maxval 255. Read: `P6` with `#` comments in the header, maxval 1 to 65535 (16-bit
  samples are scaled down). ASCII `P3` is not read.
* Pictures larger than 4096 x 4096, truncated files, damaged headers and zero-size pictures give an error dialog. A save
  goes to `name.sketch~` first and replaces the target when it is complete, so a full disk never destroys the old file.

### How it is built

`user/bin/sketchbook/`: `image.c` (the picture and the undo history), `raster.c` (lines, shapes, text and flood fill as
horizontal spans), `fileio.c` (BMP and PPM), `canvas.c` (zoom, scrolling, drawing the picture and its overlays),
`tools.c` (what each tool does, selections, text, clipboard), `panels.c` (toolbox, palette, mixer, status bar), `dialogs.c`,
`icons.c` (procedural tool icons), `main.c` (menus, shortcuts, file commands).

* Undo works on 64 x 64 pixel tiles: before something is painted, the tiles it touches are saved once per operation, and
  undo swaps them back, so a pencil stroke on a 4096 x 4096 canvas costs a few kilobytes and a whole-picture fill 64 MB
  at most. Flip, rotate and invert are recorded as the operation itself (each undoes the other); resizing keeps the old
  pixels. The history keeps up to 200 operations and about 96 MB.
* Every tool paints through the same span rasteriser that draws the live preview, so what you see while dragging is
  exactly what is painted.
* Pointer events are queued by an event hook and replayed in order each frame, so quick clicks and fast strokes keep every
  press, release and intermediate position even when several arrive between two frames.

### Tests

`tests/case_sketchbook.py` boots the desktop in QEMU and drives the program with the mouse and keyboard: drawing and
undo/redo with pixel checks, zoom and grid, the colour mixer, saving and re-reading BMP and PPM files (byte for byte through
the serial shell), bad files, dialogs and the unsaved-changes guard, selections and image operations, text and spray,
shift-constrained shapes and opening `/tmp/screenshot.ppm`. Run them with `python tests/run_all.py sketchbook`.
