# PC-98 display hardware (`pc98gfx`)

A standalone device model of the NEC PC-9801's display side, written for the "Hakurei emulator" ([PC98.md](PC98.md)):
it owns the video memory and every graphics-related I/O port, applies the hardware's side effects on each access and
draws the 640x400 picture. It knows nothing of the machine around it; the machine routes accesses to it (integration
notes at the end). Branch `agent/pc98-gfx`.

| File | What |
|------|------|
| `user/lib/pc98/include/pc98gfx.h` | the state (`Pc98Gfx`, 287,104 bytes, plain data) and the API |
| `user/lib/pc98/src/pc98gfx.c` | the model: text VRAM, character generator, planes, palettes, GRCG, EGC, two uPD7220s, renderer |
| `tests/host/pc98gfxtool.c` | a command-script driver for the tests (also builds `shared/font8x16.c` in) |
| `tests/host/test_pc98gfx.py` | 48 host tests against models written independently of the C code |

The module is freestanding (`<stdint.h>`, `<stddef.h>`, `memset`, `memcpy`; no allocation, no floating point) and is
compiled into `libpc98.a` by `tools/build.py` like the rest of `user/lib/pc98` (checked: clean with the OS flags). It
contains no ROM and no font data; glyphs come from the caller (see "Fonts").

## API

```c
static Pc98Gfx g;                          /* or malloc(sizeof g): about 280 KiB */
pc98gfx_init(&g);                          /* cleared, then pc98gfx_reset(): the state the BIOS leaves */
pc98gfx_set_font(&g, ank8x16, kanji_fn, user);            /* borrowed; any may be NULL */
pc98gfx_mem_read8/16(&g, addr), pc98gfx_mem_write8/16(&g, addr, v);   /* A0000-EFFFF */
pc98gfx_mem_claims(addr);                  /* A0000-A4FFF, A8000-BFFFF, E0000-E7FFF */
pc98gfx_port_read(&g, port, &value, width), pc98gfx_port_write(&g, port, value, width);   /* 1 = ours */
pc98gfx_set_retrace(&g, vsync, hblank);    /* the GDC status bits 5 and 6 */
pc98gfx_vsync(&g);                         /* once per frame: the blink phases */
pc98gfx_render(&g, rgb, pitch);            /* 640x400 0x00RRGGBB; _render_phase() takes the blink phases */
pc98gfx_text_dump(&g, buf);                /* 25 x (80 + '\n') + NUL: ASCII as is, the rest '?' */
pc98gfx_ppm(rgb, w, h, pitch, out, cap);   /* binary PPM bytes for eyeballing */
```

Also: `pc98gfx_colour()` (a resolved palette entry), `pc98gfx_cg_glyph()` (what the character generator holds for a
code), `pc98gfx_ank_unicode()` / `pc98gfx_ank_from_unicode()`, the `pc98gfx_fontrom_*` helpers, `pc98gfx_plane()`
(raw plane memory for debuggers), `pc98gfx_memsw()` / `pc98gfx_set_memsw()`, `pc98gfx_blink_visible()` /
`pc98gfx_cursor_visible()`. Word port accesses are the low byte at `port` and the high byte at `port + 1`; write-only
registers are not claimed on reads (the machine sees those reads as unhandled, which keeps them visible in traces).

## Conventions (three bit orders)

- **CPU / VRAM bytes:** bit 7 is the leftmost of 8 dots. A dot-ordered 16-bit value puts the even byte on top (bit 15
  = dot 0); the EGC computes in this order. Its registers that hold dots (the mask, a CPU word) are in VRAM byte order:
  the low byte covers the left 8 dots.
- **GDC words:** bit n is dot n of the word counted from the left (the dot address dAD), so a graphics GDC word is the
  bit-reversed dot-ordered value. MASK, WDAT and RDAT data use this order. The text GDC's words are character codes.
- **EGC stream order:** the order a row is processed in, left to right or (descending) right to left.

## Memory map

| Range | Contents | Behaviour |
|-------|----------|-----------|
| A0000-A1FFF | character codes, one word per cell, 4096 cells | plain RAM |
| A2000-A3FFF | attributes, the even byte of each word | plain RAM (odd bytes too); the memory switches below |
| A3FE2 + 4n (n = 0-7) | memory switches 1-8 | writes take effect only while 68h flip-flop 6 is set (`out 68h, 0Dh`) |
| A4000-A4FFF | character generator window | claimed, reads FFh (not modelled) |
| A8000 / B0000 / B8000 / E0000 | planes B, R, G, E (32 KiB each) | the page in A6h; GRCG or EGC when enabled |

A word access at an even graphics address is one EGC operation; everything else is done as bytes, low byte first.

## Ports

| Port | Direction | Behaviour |
|------|-----------|-----------|
| 60h / 62h | R W | text GDC: status / parameter; data / command |
| A0h / A2h | R W | graphics GDC: the same |
| 68h | W | mode flip-flop 1 (`0000nnnv`): 0 simple graphics, 1 monochrome (stored), 2 40 columns, 3 font select (stored), 4 hide the odd raster in 200-line mode, 5 kanji dot access (stored), 6 memory switch write, 7 display enable |
| 6Ah | W | mode flip-flop 2: 00h/01h 8/16 colours; 07h/06h permit/protect the mode change; 04h/05h GRCG/EGC mode (only while permitted); other codes counted |
| 6Ch | W | border colour (stored; the picture has no border) |
| 70h-7Ah even | W | text CRTC (PL, BL, CL, SSL, SUR, SDR): stored, not used |
| 7Ch / 7Eh | W | GRCG mode (resets the tile pointer) / tiles B, R, G, E in turn |
| A1h, A3h, A5h | W | character generator: code (cell), code (row - 20h, 0 = ANK), line and half |
| A9h | R W | the glyph byte; writes reach the user-defined rows only |
| A4h / A6h | R W | displayed page / draw page (bit 0) |
| A8h-AEh even | R W | palette: digital (two GRB codes per port) or analogue (A8h index, AAh green, ACh red, AEh blue) |
| 4A0h-4AFh | W | EGC registers (bytes or words); writes count only in EGC mode |

Port 64h (the VSYNC interrupt) stays with the machine.

## Text layer

- **Cells:** an ANK character is the code in the low byte, 0 in the high byte. A two-byte character occupies two cells
  holding the low byte JIS first byte - 20h (bit 7 set in the right-hand cell) and the high byte the JIS second byte.
  **The right half is marked by bit 7 of either byte** (2026-11-03, found with TH02): NEC's DOS writes `23h 66h` then `A3h 66h` (bit 7 of the low byte), the
  games TH02 to TH05 write their user-defined characters (the HUD text, drawn as 76xx/77xx cells with the ASCII code as the cell number) as `56h c` then
  `56h c|80h` (bit 7 of the high byte), and both are right halves on the machine, so the model takes `(low | high) & 80h`; with the low byte's bit alone every one
  of those characters came out as two left halves ("<<<<" shapes where the HUD text should be). Which bit the hardware really looks at is not known: the rule is
  the one both programs agree with. Test: `Text.test_right_half_is_marked_by_bit_7_of_either_byte`.
  Rows 76h and 77h (low byte 56h, 57h) come from the user-defined character RAM, the rest from the kanji callback.
- **Attributes** (`GRBVUrbs`): bit 0 shown, 1 blink, 2 reverse, 3 underline, 4 vertical line (simple graphics when
  flip-flop 0 is set), 5-7 blue, red, green. Per dot line of a cell the renderer takes the glyph line (blank beyond
  line 15), clears it when the cell is secret or blinked off, sets the whole line for the underline on the last line
  of the row, adds the leftmost dot for the vertical line, inverts for reverse, then inverts again on the cursor's
  lines. Dots that end up set are drawn in the attribute's colour (the eight fixed colours, not the palette) over the
  graphics; clear dots are transparent.
- **Simple graphics:** the code's bits light a 2 x 4 grid of blocks (bits 0-3 the left column top to bottom, bits 4-7
  the right one).
- **Geometry from the text GDC:** CSRFORM's lines-per-row + 1 is the row height (16 for 25 rows, 20 for 20 rows),
  PITCH the cells per row of memory, the four display partitions in PRAM (start address, length in lines) the
  scrolling. Display addresses wrap at 4096 cells. 40-column mode draws each cell twice as wide.
- **Cursor:** CSRFORM bit 7 of the first byte shows it, on lines CTOP..CBOT of the cell at the text GDC's EAD.
  Blinking: on for BR frames, off for BR (BR = 0 counts as 32); steady when bit 5 of CSRFORM's second byte is set.
- **Attribute blink:** on for 32 frames, off for 32 (`pc98gfx_vsync()` counts frames).
- **Text dump:** 25 x 80 cells from partition 1's start address with the text GDC's pitch.

## Character generator and fonts

- `A3h` = 0 reads the ANK glyph of code `A1h`; otherwise `A3h` (bit 7 ignored) and `A1h` name a two-byte character,
  `A5h` bits 0-3 the line and bit 5 the half (set = left). `A9h` returns the glyph byte; writing `A9h` stores into the
  user-defined RAM (rows 76h and 77h, cells 00h-7Fh: 256 characters of 16x16) and is ignored for ROM characters.
- `pc98gfx_set_font(g, ank, kanji, user)`: `ank` is 256 glyphs of 16 bytes (MSB = left dot); `kanji(user, jis, out)`
  fills 32 bytes (16 lines of the left half, then 16 of the right half) or returns 0 (blank). Without fonts the text
  layer draws only the attribute decorations.
- **The repository's font** (`shared/font8x16.c`, code points, not an ANK table) becomes one through
  `pc98gfx_ank_from_unicode(table, font8x16_glyph)`: 20h-7Eh as JIS X 0201 (5Ch is the yen sign, which the font has;
  7Eh the overline, which it lacks, so the macron stands in), A1h-DFh the half-width katakana (absent from that font:
  they show its "missing" box), the rest blank. The NEC graphic characters (80h-9Fh, E0h-FFh) are not mapped: their
  assignment was not verified. There is no repository kanji font yet (blank kanji).
- **A user's own FONT.ROM** (288,768 bytes; the layout was worked out from the owner's file by structure alone:
  `pc98gfx_fontrom_valid`, `pc98gfx_fontrom_ank16`, `pc98gfx_fontrom_kanji` as the callback with the image as `user`):
  0000h 256 ANK glyphs of 8x8, 0800h 256 of 8x16, 1800h kanji for JIS rows 21h-7Ch with 96 cells (20h-7Fh) of 32 bytes
  each, left half then right half. Rows 29h-2Bh hold half-width characters (left half only), rows 76h-77h are empty
  (on the machine they are RAM). The image is read at run time and never enters the repository.

## Graphics layer and palette

- The displayed page (A4h), planes B R G E as colour bits 0-3. 8-colour mode uses bits 0-2 through the digital
  palette: colour n's GRB code is the high nibble of AEh, ACh, AAh, A8h for n = 0-3 and the low nibble for n = 4-7.
  16-colour mode: the analogue palette's 4-bit levels times 17. Power-on: the identity digital palette; analogue
  eight dark (level 7) and eight bright (15) colours with grey 4 at index 8 (our choice).
- Geometry from the graphics GDC: two partitions (start address in words, length in lines; master.lib-style
  vertical scrolling), PITCH words per line, addresses wrapping at 16K words. **200-line mode:** CSRFORM's
  lines-per-row + 1 rasters per line of memory; flip-flop 4 blanks all but the first.
- Flip-flop 7 clear blanks the whole picture; each GDC's display enable (RESET/SYNC/BCTRL/START) its layer.

## GRCG (7Ch, 7Eh)

7Ch bit 7 on, bit 6 RMW, bits 0-3 disable planes B R G E. While on, the plane in the address does not matter:
**TDW** (bit 6 clear, writes) stores each enabled plane's tile and ignores the CPU's byte; **RMW** (writes) puts the
tile where the CPU's byte has ones; **TCR** (bit 6 clear, reads) returns ones where every enabled plane equals its
tile. Reads in RMW mode are plain reads of the addressed plane. Words are two bytes. The draw page applies.

## EGC (4A0h-4AEh)

Active when 7Ch bit 7 is set and flip-flop 2 is in EGC mode (`out 6Ah`: 07h, 05h, 06h). Registers:

| Reg | Bits used |
|-----|-----------|
| 4A0h access | 0-3: write-protect planes B R G E |
| 4A2h | 0-3: planes taking part in compare reads; 8-9: the plane plain reads return; 13-14: pattern operand (00 pattern register, 01 background, 10 foreground colour) |
| 4A4h | 0-7 ROP; 8-9 pattern register load (01 from VRAM reads, 10 from the destination of writes); 10 the shifter is fed by CPU writes (else by VRAM reads); 11-12 what is written (00 shifter output, 01 ROP result, 10 pattern); 13 compare read |
| 4A6h / 4AAh | foreground / background colour (bits 0-3) |
| 4A8h | bit mask (VRAM byte order) |
| 4ACh | 0-3 source bit address, 4-7 destination bit address, 12 descending |
| 4AEh | bit length - 1 (12 bits) |

- **ROP:** bit k of the code is the result for S = bit 2 of k, D = bit 1, P = bit 0 (F0h source, CCh destination,
  AAh pattern, ACh "source selects pattern or destination").
- **Shifter:** a queue of dots per plane (the newest 64 kept). Each feeding access enters its dots in stream order;
  the first of a row enters from the source bit address on. Each write takes the dots it needs: the first write of a
  row from the destination bit address on, the last only up to the bit length. **A write that finds too few dots
  writes nothing** (published hardware behaviour: the first write of a row whose source bit address is ahead of the
  destination's, unless the program reads one extra word first; the row then lands 16 dots to the right). Once the
  row's bit length has been written the shifter starts over by itself, so one 4ACh/4AEh setup serves a whole
  rectangle. Writing 4ACh or 4AEh also resets it.
- **Writes:** the result (per 4A4h bits 11-12) lands on the planes 4A0h does not protect, under the mask and the
  dots the shifter supplied. **Reads:** compare reads return ones where every selected plane equals the pattern
  operand; plain reads return the read plane.
- **Byte accesses** are halves of their word (bit addresses stay word-relative), so a word access arriving as two
  byte accesses (low byte first: what the machine's 8-bit CPU bus delivers) behaves as the word for rows that run left
  to right; a row that ends in the even byte leaves the odd byte outside every row. **Descending rows need real word
  accesses.**

## The two uPD7220s

The whole command protocol of both chips: RESET (00h, 01h, 09h) and SYNC (0Eh/0Fh) with their parameters, BCTRL
(0Ch/0Dh), START (6Bh), VSYNC (6Eh/6Fh), ZOOM (46h), PITCH (47h), CSRW (49h), MASK (4Ah), CSRFORM (4Bh), PRAM
(70h-7Fh), FIGS (4Ch, with the data sheet's defaults D = 8, D2 = 8, D1 = DM = -1 on every FIGS), FIGD (6Ch), GCHRD
(68h), WDAT (20h-3Fh with bit 2 clear: TYPE word/low/high, MOD replace/complement/clear/set), RDAT (A0h-BFh with bit 2
clear), CURD/CSRR (E0h: EAD in three bytes, then the MASK register), LPRD (C0h: three zeros). DMAR/DMAW are counted
and ignored (no DMA controller here); unknown codes are counted. Commands run at once: the status has FIFO empty set,
data ready while an answer or RDAT data waits, VSYNC and HBLANK from `pc98gfx_set_retrace()`; a new command drops an
unread answer.

- **Memory as each chip sees it:** the text GDC (character mode) addresses the 8192 words of text VRAM (codes
  0000h-0FFFh, attributes 1000h-1FFFh, memory switch protection included); the graphics GDC the planes of the draw
  page, 4000h words each (B 0000h, R 4000h, G 8000h, E C000h). GDC writes do not go through the GRCG or EGC.
- **WDAT:** each data unit is written DC + 1 times, the address moving along DIR by words; the graphics GDC writes
  under MASK, the text GDC whole words (or the byte TYPE names). **RDAT** delivers DC + 1 words.
- **Drawing (graphics GDC):** directions 0 down, 1 down-right, 2 right, ... 6 left, 7 down-left; octant k lies between
  directions k and k + 1. Each dot is a read-modify-write with the last WDAT's MOD and the next bit of the pattern in
  PRAM bytes 8-9 (bit 0 first for each figure). **Lines** take the data sheet's parameters (DC = |dI|, D = 2|dD| -
  |dI|, D2 = 2(|dD| - |dI|), D1 = 2|dD|): DC + 1 dots, a diagonal step while D is not negative. **Arcs** (DC = r sin
  end angle, D = r - 1, DM = r sin start angle) start where the circle's tangent is the octant's axis and pick the
  dot nearest the true circle; steps before DM are not drawn. **Rectangles** (DC = 3, D = A - 1, D2 = B - 1, DM =
  A - 1) draw four sides turning two direction steps each time and end where they started. **GCHRD** draws DC + 1
  lines of D dots (8 when 0) from the 8x8 pattern in PRAM 8-15 (byte 8 + j is line j, bit i dot i), each dot zoomed by
  ZOOM's low nibble + 1, dots along DIR and lines two steps clockwise from it. FIGD with no figure flag draws DC + 1
  dots along DIR. After a figure, the cursor is on its last dot and MASK holds that dot. The text GDC draws no figures.
- CSRW on the graphics GDC sets the dot address and MASK to that dot; on the text GDC it leaves MASK alone.

## Verified, unverified, missing

**Checked against public documentation** (the port tables of a PC-98 programming tutorial and a published account of
the EGC's behaviour on real machines): the attribute layout, the 7Ch layout with active-low plane bits, the 68h
flip-flop numbering, the 6Ah codes, the analogue palette ports, the EGC shifter re-arming after the bit length and the
"first write writes nothing" case. The 7220 command codes, parameter formats and drawing parameters follow its data
sheet as remembered. **Unverified** (from memory or our own choice; each is one place in `pc98gfx.c`):

1. A5h bit 5: set = left half (`CG_LEFT`). If the user-defined characters of TH02-TH05 show their halves swapped, flip it.
2. User-defined characters: rows 76h-77h with cells 00h-7Fh; the right-half mark (bit 7 of the low byte) as described.
3. Secret and blink-off clear only the pattern: underline, vertical line and reverse still show (a secret reversed
   cell is a solid block). The underline on the row's last line, the vertical line on its leftmost dot.
4. Blink rates (32 + 32 frames; the cursor on BR, off BR), the steady bit's position, BR = 0 meaning 32.
5. Simple-graphics block layout; 40-column mode (consecutive cells, double width); glyph lines at the top of 20-line rows.
6. 200-line mode through the graphics GDC's CSRFORM, and flip-flop 4's polarity. (None of TH01-TH05 uses 200 lines.)
7. Flip-flop 7 blanks text and graphics alike.
8. Palette ports and A4h/A6h are readable; the analogue palette's power-on values.
9. The memory switch addresses and their write protection; odd attribute bytes as RAM.
10. GRCG: RMW-mode reads plain, TCR with no plane enabled returns FFh, the tile pointer cycles all four tiles.
11. EGC: every bit meaning in the register table above except 4ACh's and 4AEh's roles; that its registers ignore
    writes outside EGC mode; compare reads against the pattern operand; the write source 00 = shifter output; pattern
    loads (01 reads, 10 the destination of writes); bit addresses counted from the left in both directions; the
    64-dot shifter; the reset values; byte accesses as half words.
12. GDC: the direction numbering and the dot order of graphics GDC words (both decide where WDAT and figures land);
    the text GDC's address map; GDC drawing into the draw page (A6h); WDAT repeating each unit DC + 1 times; the
    line's tie (D = 0 steps diagonally); the rectangle's turning direction; arc dots (the chip's own recurrence may
    differ by a dot); GCHRD's byte and bit order and raster (not zigzag) order; FIGD without flags; the pattern
    restarting with each figure; FIFO empty staying set while data waits; display partitions with LEN 0 reaching the
    bottom and blank lines below the last partition.
13. The ANK mapping of 7Eh (overline).

**Not modelled:** the character generator window (A4000h, kanji dot access), DMA, the GDCs' display zoom, wide and
image bits of partitions, mixed-mode partitions, interlace, the SYNC timing (the picture is always 640x400 at the
module's pace), light pen; the text CRTC (smooth scrolling, cursor line registers); monochrome graphics; the old
200-line text fonts; PC-9821 modes (256 colours, 31 kHz, 6Eh); the GRCG/EGC during GDC drawing; drawing time (never
"drawing in progress"); EGC descending rows through split byte accesses; a repository kanji font.

## Tests

`python tools/hosttest.py discover -s tests/host -p "test_pc98gfx.py" -v` (WSL on the owner's PC; 48 tests, about 15 s).
The expected values come from Python models written from the descriptions above, not from the C code: planes and the
GRCG and EGC as operations on single dots (every ROP code with random operands, masks and protected planes, through
words and through split bytes), EGC block copies as what a copy means (each destination dot takes its source dot;
ascending, descending with overlap, split bytes, between pages, CPU-fed rows) plus the "missing prefill read" case,
the 7220's figures from their geometry (closed-form lines in every octant, circles by rounding, hand-computed lines,
a rectangle, characters), the text layer from the attribute definitions, and whole pictures from a per-pixel renderer.
Three CRC-32 regression hashes of fixed scenes, a PPM round trip, the struct size. `test_owners_font_rom` runs only
where the owner's FONT.ROM exists and checks structure only (it ran and passed on the owner's PC).
Mutation check: seven deliberate bugs (the GRCG tile write, the EGC's empty-shifter write, the split-word tail, the
line tie, the digital palette order, the CG half bit, reverse video) each fail the tests aimed at them.

Speed (gcc -O2 in WSL, a busy shared PC, so a range): rendering a frame with text, kanji and graphics 0.47-1.5 ms;
a GRCG fill of a whole plane through byte writes 80-175 us; an EGC copy of 16,000 words 0.7-2.8 ms.

## Integration notes for `pc98mach.c`

**Done 2026-11-03** (branch `agent/pc98-integrate`): points 1 to 8 below are in `pc98mach.c`/`pc98.h`; point 9 for INT 18h AH=14h and 1Ah, the
text fill and the memory switches (the other display calls stay as they were). The word hooks of point 4 are `X86Bus.read16/write16`, used by both
CPU cores. The three `test_pc98.py` expectations were changed as point 8 says (the HBLANK status bit is set late in each line, so power-on stays 24h).
TH01 draws its logo and title through the module; the stray marks on the title picture turned out to be BIOS slips, not the module's ([PC98.md](PC98.md), "M5 and M6").

1. **State:** embed `Pc98Gfx gfx` in `Pc98`. It replaces `tvram`, `gvram`, `gdc[2]`, `mode_ff1`, `mode_ff2_analog`,
   `border`, `disp_page`, `draw_page` and the palette fields (and with them `pc98dev.c`'s GDC bookkeeping, about
   280 KiB of duplicated memory). `pc98_init()` calls `pc98gfx_init(&m->gfx)` (spaces with E1h, text on, graphics
   off: the state the skeleton starts in) and `pc98gfx_set_retrace(&m->gfx, 1, 0)` to keep its VSYNC-at-power-on.
2. **Fonts:** the application or harness owns the tables. ANK from the repository's font:
   `static uint8_t ank[4096]; pc98gfx_ank_from_unicode(ank, font8x16_glyph); pc98gfx_set_font(&m->gfx, ank, 0, 0);`
   (`font8x16.c` is compiled into libkappa). With the owner's FONT.ROM loaded at run time:
   `pc98gfx_set_font(&m->gfx, pc98gfx_fontrom_ank16(rom), pc98gfx_fontrom_kanji, rom)` after `pc98gfx_fontrom_valid`.
3. **Memory:** in `pc98_read8`/`pc98_write8`, after the RAM check: `if (pc98gfx_mem_claims(a)) return
   pc98gfx_mem_read8(&m->gfx, a);` (and the write). A5000-A7FFF, C0000-DFFFF and E8000 up stay the machine's (FFh).
4. **Word accesses:** `X86Bus` is byte-wide, which is fine for everything except EGC rows that run right to left.
   Optional `read16`/`write16` bus callbacks (the CPU core's file, `x86emu.h`) routed to `pc98gfx_mem_read16/16` for
   even addresses would close that gap; a 32-bit access is two word accesses, low word first.
5. **Ports:** in `pc98_in8`/`pc98_out8`, before the machine's own cases: `uint32_t v; if
   (pc98gfx_port_read(&m->gfx, port, &v, 1)) return (uint8_t)v;` and `if (pc98gfx_port_write(&m->gfx, port, v, 1))
   return;`, and delete the machine's cases for 60h, 62h, 68h, 6Ah, 6Ch, 70h-7Ah, A0h, A2h, A4h, A6h, A8h-AEh. 64h
   stays. New to the machine: 7Ch, 7Eh, A1h, A3h, A5h, A9h, 4A0h-4AFh.
6. **Time:** in `pc98_advance()`, call `pc98gfx_set_retrace(&m->gfx, vs, 0)` when VSYNC changes and
   `pc98gfx_vsync(&m->gfx)` once at the start of each VSYNC (next to the CRTV interrupt). The status bits now come
   from the module.
7. **Output:** `pc98_render()` becomes `pc98gfx_render()` (the font argument moves to `pc98gfx_set_font()`);
   `pc98_text_dump()` becomes `pc98gfx_text_dump()` (same format; it follows the display start address).
8. **`tests/host/test_pc98.py` after the switch:** `gdc1_ignored` disappears (WDAT is carried out now: the test's
   single 0FFh parameter waits for its high byte); 7Eh is claimed, so `unhandled` drops to 1 with 7Ch last; the
   render test needs its synthetic font set through `pc98gfx_set_font()`; the boot demos should be unchanged
   (VSYNC through `set_retrace`, CSRR's answer bytes as before).
9. **A BIOS layer** maps INT 18h onto the module: text on/off and the cursor (text GDC BCTRL, CSRW with the cell
   index, CSRFORM), graphics on/off and 400/200 lines (graphics GDC BCTRL, CSRFORM), display areas (PRAM partitions),
   font reads (`pc98gfx_cg_glyph`), user character definition (the CG ports or `gaiji[]`), memory switches
   (`pc98gfx_set_memsw`). Games learn about the EGC from the BIOS work area, which the BIOS layer fills in;
   `has_egc = 0` models a machine without one.

## Sources

The uPD7220/82720 GDC data sheet (command set, parameter formats, drawing parameters); the publicly documented PC-98
memory and I/O maps (a programming tutorial's port tables: attributes, 7Ch, 68h, 6Ah, palette ports); the ReC98
project's published description of the EGC shifter on real hardware (prose, not code); the owner's FONT.ROM for its
layout only (by structure: which blocks are empty, which halves are used). No emulator's source was used.
