/*
 * pc98gfx: the display hardware of a NEC PC-9801 as software sees it, as one standalone device model (docs/PC98_GFX.md).
 *
 * It owns the video memory and every graphics-related I/O port, applies the hardware's side effects on each access and
 * draws the 640x400 picture:
 *
 *  - text VRAM (A0000-A1FFF character codes, A2000-A3FFF attributes, the memory switches near its end) and the character
 *    generator ports (A1h, A3h, A5h, A9h) with the RAM for user-defined characters ("gaiji");
 *  - four graphics planes (B A8000, R B0000, G B8000, E E0000; 32 KiB each) in two pages (A6h: the page the CPU and the
 *    graphics GDC draw into, A4h: the page shown);
 *  - the 8-colour digital and the 16-colour analogue palette (A8h-AEh) and the mode flip-flops (68h, 6Ah), border (6Ch);
 *  - the GRCG (7Ch mode, 7Eh tiles): tile data write, tile compare read, read-modify-write;
 *  - the EGC (4A0h-4AFh): plane access, raster operations over source, destination and pattern, the bit-address shifter
 *    for block copies, compare reads;
 *  - both uPD7220 display controllers (text 60h/62h, graphics A0h/A2h): the whole command protocol, the display
 *    partitions (scrolling), the cursor, WDAT/RDAT, and the drawing processor's lines, arcs, rectangles and characters;
 *  - the renderer: graphics (palette applied, the displayed page) under the text layer (fonts through an interface).
 *
 * Nothing here depends on the machine around it (include/pc98.h): the machine routes memory accesses in A0000-EFFFF and
 * the ports listed in docs/PC98_GFX.md to these functions, calls pc98gfx_vsync() once per frame and tells the module
 * where the beam is (pc98gfx_set_retrace). The structure is plain data, about 290 KiB, with no pointers it owns: no
 * malloc, no floating point; <stdint.h>, <stddef.h>, memset and memcpy only, so it builds freestanding against libgap.
 *
 * No ROM and no font data are part of this module: the ANK and kanji glyphs come from the caller (pc98gfx_set_font).
 * The repository's own 8x16 font can supply the ANK table through pc98gfx_ank_from_unicode(); a user's own FONT.ROM
 * image (read at run time, never shipped) through the pc98gfx_fontrom_* helpers.
 *
 *      static Pc98Gfx g;                                   // or malloc(sizeof g)
 *      pc98gfx_init(&g);
 *      pc98gfx_ank_from_unicode(ank, font8x16_glyph);       // a 4096-byte table the caller keeps
 *      pc98gfx_set_font(&g, ank, NULL, NULL);
 *      ...  pc98gfx_mem_write8(&g, 0xA0000, 'A');  pc98gfx_port_write(&g, 0xA2, 0x0D, 1);  ...
 *      pc98gfx_render(&g, pixels, 640);                     // 640x400, 0x00RRGGBB
 */
#ifndef GAP_PC98GFX_H
#define GAP_PC98GFX_H

#include <stddef.h>
#include <stdint.h>

/* ---- memory map (physical addresses) */
#define PC98GFX_TVRAM           0xA0000u    /* character codes: one 16-bit word per cell, 4096 cells */
#define PC98GFX_TVRAM_ATTR      0xA2000u    /* attributes: the even byte of each word */
#define PC98GFX_TVRAM_END       0xA4000u
#define PC98GFX_CGWIN           0xA4000u    /* character generator window: not modelled, reads FFh */
#define PC98GFX_CGWIN_END       0xA5000u
#define PC98GFX_PLANE_B         0xA8000u
#define PC98GFX_PLANE_R         0xB0000u
#define PC98GFX_PLANE_G         0xB8000u
#define PC98GFX_PLANE_E         0xE0000u    /* the fourth ("intensity") plane of the 16-colour machines */
#define PC98GFX_PLANE_SIZE      0x8000u     /* 640 x 400 / 8 = 32000 bytes used, 32768 present */
#define PC98GFX_WIDTH           640
#define PC98GFX_HEIGHT          400
#define PC98GFX_TEXT_COLS       80
#define PC98GFX_TEXT_ROWS       25
/* The memory switches (battery-backed settings DOS reads) live in the attribute plane at A3FE2h + 4n, n = 0..7; they
 * take writes only while mode flip-flop 1 bit 6 (68h: 0Dh) permits it. */
#define PC98GFX_MEMSW           0xA3FE2u
#define PC98GFX_TEXT_DUMP_SIZE  (PC98GFX_TEXT_ROWS * (PC98GFX_TEXT_COLS + 1) + 1)

/* Text attribute bits (the even byte at A2000h + 2 * cell). */
#define PC98GFX_ATTR_SHOW       0x01        /* "ST": 0 = secret, the character pattern is not shown */
#define PC98GFX_ATTR_BLINK      0x02
#define PC98GFX_ATTR_REVERSE    0x04
#define PC98GFX_ATTR_UNDERLINE  0x08
#define PC98GFX_ATTR_VLINE      0x10        /* vertical line; simple graphics when mode flip-flop 1 bit 0 is set */
#define PC98GFX_ATTR_BLUE       0x20
#define PC98GFX_ATTR_RED        0x40
#define PC98GFX_ATTR_GREEN      0x80

/* Mode flip-flop 1 (port 68h: value 0000nnnv sets flip-flop n to v); kept as bit n of Pc98Gfx.mode1. */
#define PC98GFX_FF1_SEMIGRAPHICS 0x01       /* attribute bit 4 = simple graphics instead of the vertical line */
#define PC98GFX_FF1_MONO        0x02        /* graphics monochrome mode: stored, not modelled */
#define PC98GFX_FF1_40COL       0x04        /* text 40 columns (characters drawn twice as wide) */
#define PC98GFX_FF1_FONT7x13    0x08        /* font select for the old 200-line text mode: stored only */
#define PC98GFX_FF1_HIDE_ODD    0x10        /* 200-line graphics: the second raster of each line pair is blank */
#define PC98GFX_FF1_KAC_DOT     0x20        /* kanji access: dot access instead of code access: stored only */
#define PC98GFX_FF1_MEMSW_WRITE 0x40        /* memory switches writable */
#define PC98GFX_FF1_DISPLAY     0x80        /* display enable (the whole picture) */

/* GRCG mode register (port 7Ch). */
#define PC98GFX_GRCG_ON         0x80
#define PC98GFX_GRCG_RMW        0x40        /* read-modify-write; clear: tile data write / tile compare read */
/* bits 0-3: plane B, R, G, E *disabled* when set */

/* EGC registers, as Pc98GfxEgc.reg[(port - 4A0h) / 2]. */
enum {
    PC98GFX_EGC_ACCESS = 0,     /* 4A0h plane access: bits 0-3 write-protect planes B R G E */
    PC98GFX_EGC_FGBG,           /* 4A2h read/compare planes (0-3), read plane (8-9), pattern source (13-14) */
    PC98GFX_EGC_OPE,            /* 4A4h ROP (0-7), pattern load (8-9), CPU-fed shifter (10), write source (11-12), compare read (13) */
    PC98GFX_EGC_FG,             /* 4A6h foreground colour (0-3) */
    PC98GFX_EGC_MASK,           /* 4A8h bit mask (VRAM byte order: the low byte covers the left 8 dots) */
    PC98GFX_EGC_BG,             /* 4AAh background colour (0-3) */
    PC98GFX_EGC_SFT,            /* 4ACh source bit address (0-3), destination bit address (4-7), descending (12) */
    PC98GFX_EGC_LENG,           /* 4AEh bit length - 1 (0-11) */
};

/* ---- one uPD7220 GDC */
typedef struct {
    uint8_t cmd;                /* the command the next parameters belong to */
    uint8_t nparam;             /* parameters received for it so far */
    uint8_t display_on;
    uint8_t sync[8];            /* RESET/SYNC parameters */
    uint8_t pitch;              /* words per line of display memory */
    uint8_t zoom;               /* display zoom - 1 (high nibble), character drawing zoom - 1 (low nibble) */
    uint8_t csrform[3];         /* CSRFORM (CCHAR): cursor display, lines per row, cursor lines, blink */
    uint8_t pram[16];           /* parameter RAM: display partitions (SAD, LEN), the drawing pattern */
    uint16_t mask;              /* MASK register, in GDC bit order (bit n = dot n of a word) */
    uint32_t ead;               /* execute address (18 bits, words) */
    uint8_t dad;                /* dot address within the word (0-15) */
    uint8_t mod;                /* the RMW logic of the last WDAT: 0 replace, 1 complement, 2 clear, 3 set */
    uint8_t wtype;              /* the TYPE of the last WDAT: 0 word, 2 low byte, 3 high byte */
    uint8_t wlow;               /* WDAT: the low byte of a word waiting for its high byte */
    uint8_t wlow_valid;
    /* FIGS drawing parameters (14-bit fields) */
    uint8_t figs_flags;         /* SL R A GC L (bits 7-3) */
    uint8_t dir;                /* drawing direction 0-7 */
    uint16_t dc, d, d2, d1, dm;
    /* read side: CSRR/LPRD answers in a small FIFO, RDAT data produced on demand */
    uint8_t fifo[8];
    uint8_t fifo_len, fifo_pos;
    uint32_t rdat_left;         /* words RDAT still has to deliver */
    uint8_t rdat_type;
    uint8_t rdat_high;          /* the high byte of the current word, when still owed */
    uint8_t rdat_high_valid;
    /* counters, for debugging and tests */
    uint32_t commands;          /* command bytes received */
    uint32_t unknown;           /* commands this model does not know (accepted, ignored) */
    uint32_t dma;               /* DMAR/DMAW: no DMA controller here, ignored */
    uint32_t dots;              /* dots plotted by the drawing processor */
    uint32_t words_written;     /* WDAT word writes */
} Pc98GfxGdc;

/* ---- the GRCG */
typedef struct {
    uint8_t mode;               /* 7Ch */
    uint8_t tile[4];            /* B R G E */
    uint8_t tile_next;          /* which tile the next 7Eh write fills */
} Pc98GfxGrcg;

/* ---- the EGC */
typedef struct {
    uint16_t reg[8];            /* 4A0h-4AEh */
    uint16_t pat[4];            /* the pattern register, one word per plane, in dot order (bit 15 = leftmost dot) */
    uint64_t q[4];              /* the shifter: up to 64 dots per plane in stream order, the oldest at bit 63 */
    uint8_t qn;                 /* dots in the shifter (the same for every plane) */
    uint8_t in_started;         /* a unit has entered the shifter in this row */
    uint8_t out_started;        /* a unit has left it in this row */
    uint16_t out_done;          /* dots written in this row */
    uint16_t tail;              /* a row ended in the even byte of a word: its offset + 1 (the odd byte belongs to no row) */
    uint32_t underruns;         /* writes that found too few dots in the shifter, so wrote nothing */
} Pc98GfxEgc;

/* A kanji glyph source: fills 32 bytes (16 lines of the left 8 dots, then 16 lines of the right 8 dots, MSB = left dot)
 * for a JIS X 0208 code (first byte 21h-7Eh in the high byte, second byte in the low byte); returns 0 when it has no
 * glyph (the character is then blank). */
typedef int (*Pc98GfxKanjiFn)(void *user, uint16_t jis, uint8_t glyph[32]);

typedef struct Pc98Gfx {
    uint8_t tvram[PC98GFX_TVRAM_END - PC98GFX_TVRAM];  /* A0000-A3FFF */
    uint8_t gvram[2][4][PC98GFX_PLANE_SIZE];            /* [page][B, R, G, E] */
    uint8_t gaiji[2][128][32];  /* user-defined characters, JIS rows 76h and 77h, cells 00h-7Fh */

    Pc98GfxGdc gdc[2];          /* [0] text (60h/62h, character mode), [1] graphics (A0h/A2h, graphics mode) */
    Pc98GfxGrcg grcg;
    Pc98GfxEgc egc;

    uint8_t mode1;              /* mode flip-flop 1 (68h), bit n = flip-flop n */
    uint8_t analog;             /* mode flip-flop 2 (6Ah) 00h/01h: 16-colour analogue palette */
    uint8_t egc_mode;           /* 6Ah 04h/05h: EGC (extended) mode instead of the GRCG */
    uint8_t egc_permit;         /* 6Ah 06h/07h: the EGC mode bit may be changed */
    uint8_t has_egc;            /* the machine has an EGC (VX and later); 1 after init */
    uint8_t border;             /* 6Ch */
    uint8_t crtc[6];            /* text CRTC 70h-7Ah: PL, BL, CL, SSL, SUR, SDR (stored, not used) */
    uint8_t disp_page, draw_page;   /* A4h, A6h */
    uint8_t pal_digital[4];     /* A8h AAh ACh AEh in 8-colour mode: two GRB nibbles each */
    uint8_t pal_index;          /* 16-colour mode: A8h */
    uint8_t pal_analog[16][3];  /* G, R, B, 4 bits each */

    /* character generator (A1h code low, A3h code high, A5h line and half, A9h pattern data) */
    uint8_t cg_code_lo, cg_code_hi, cg_line;

    /* beam state (from the machine) and frame counter */
    uint8_t vsync, hblank;
    uint32_t frame;

    /* fonts: borrowed, never freed or written */
    const uint8_t *ank;         /* 256 glyphs of 16 bytes (MSB = left dot), or NULL: blank */
    Pc98GfxKanjiFn kanji;
    void *kanji_user;

    /* counters */
    uint32_t ff2_unknown;       /* 6Ah writes with a code this model does not know */
    uint8_t ff2_last_unknown;
} Pc98Gfx;

/* ---- set-up */
/* Everything cleared, then pc98gfx_reset(). */
void pc98gfx_init(Pc98Gfx *g);
/* The state the BIOS leaves before booting (docs/PC98_GFX.md has the list): text screen cleared (spaces, white,
 * shown; the memory switches untouched), graphics planes cleared, text GDC on (80 words per line, 16-line rows, cursor
 * off), graphics GDC off (40 words per line), 8-colour mode with the identity palette, GRCG and EGC off, page 0.
 * The fonts set with pc98gfx_set_font() and has_egc are kept. */
void pc98gfx_reset(Pc98Gfx *g);
/* Borrows the glyph sources; any may be NULL. ank8x16: 4096 bytes, glyph c at c * 16. */
void pc98gfx_set_font(Pc98Gfx *g, const uint8_t *ank8x16, Pc98GfxKanjiFn kanji, void *user);

/* ---- the CPU side */
/* Whether a physical address belongs to this module: A0000-A4FFF, A8000-BFFFF, E0000-E7FFF. */
int pc98gfx_mem_claims(uint32_t addr);
/* Memory accesses (addresses outside the claimed ranges read FFh and drop writes). Word accesses at even graphics
 * addresses reach the EGC as one 16-bit operation; anything else is done as bytes, low byte first. */
uint8_t pc98gfx_mem_read8(Pc98Gfx *g, uint32_t addr);
uint16_t pc98gfx_mem_read16(Pc98Gfx *g, uint32_t addr);
void pc98gfx_mem_write8(Pc98Gfx *g, uint32_t addr, uint8_t v);
void pc98gfx_mem_write16(Pc98Gfx *g, uint32_t addr, uint16_t v);
/* Port accesses (width 1 or 2; a word is the low byte at `port` and the high byte at port + 1). Return 1 when the
 * port is this module's (*value set on reads), 0 when not (nothing changed; the machine handles it). Write-only
 * registers are not claimed on reads. */
int pc98gfx_port_read(Pc98Gfx *g, uint16_t port, uint32_t *value, int width);
int pc98gfx_port_write(Pc98Gfx *g, uint16_t port, uint32_t value, int width);

/* ---- time */
/* Once per frame, at the start of the vertical retrace: the frame counter (blink phases). */
void pc98gfx_vsync(Pc98Gfx *g);
/* Where the beam is: the GDC status bits 5 (VSYNC) and 6 (HBLANK) of both GDCs. */
void pc98gfx_set_retrace(Pc98Gfx *g, int vsync, int hblank);
/* The blink phases for the current frame: attribute blink (on for 32 frames, off for 32), cursor (per CSRFORM). */
int pc98gfx_blink_visible(const Pc98Gfx *g);
int pc98gfx_cursor_visible(const Pc98Gfx *g);

/* ---- output */
/* The displayed picture: 640 x 400 pixels of 0x00RRGGBB, `pitch` pixels per row. */
void pc98gfx_render(const Pc98Gfx *g, uint32_t *rgb, int pitch);
/* The same with the blink phases given (1 = shown): attribute blink, cursor. */
void pc98gfx_render_phase(const Pc98Gfx *g, uint32_t *rgb, int pitch, int blink_visible, int cursor_visible);
/* The resolved colour of graphics colour index 0-15 (8-colour mode: index & 7 through the digital palette). */
uint32_t pc98gfx_colour(const Pc98Gfx *g, unsigned index);
/* The text VRAM as 25 lines of 80 characters plus '\n' (PC98GFX_TEXT_DUMP_SIZE bytes with the NUL), from the display
 * start address of the first text partition: ASCII codes 20h-7Eh as they are, secret cells and control codes as
 * spaces, anything else (kanji halves, kana, graphic characters) as '?'. */
void pc98gfx_text_dump(const Pc98Gfx *g, char *buf);
/* A binary PPM (P6) of a 0x00RRGGBB picture into out: returns the size it needs, and writes only when cap is enough. */
size_t pc98gfx_ppm(const uint32_t *rgb, int width, int height, int pitch, uint8_t *out, size_t cap);

/* ---- fonts */
/* The Unicode code point the PC-98 shows for an ANK code: JIS X 0201 (20h-7Eh with the yen sign at 5Ch and the
 * overline at 7Eh, half-width katakana A1h-DFh); 0 for codes this table does not map (00h-1Fh, 7Fh-A0h, E0h-FFh: the
 * NEC graphic characters, whose assignment is not verified here). */
uint32_t pc98gfx_ank_unicode(uint8_t code);
/* Builds a 256 x 16 ANK table from a code point lookup such as shared/font8x16.c's font8x16_glyph (which returns its
 * "missing" box for code points it lacks; 7Eh falls back to the macron when the overline is missing). Unmapped codes
 * stay blank. Returns the number of codes filled. */
int pc98gfx_ank_from_unicode(uint8_t out[4096], const unsigned char *(*glyph)(unsigned cp));
/* A glyph of the character generator as software reads it through A9h: the code as in text VRAM (low byte: JIS
 * first byte - 20h, or the ANK code with the high byte 0), 32 bytes as for Pc98GfxKanjiFn (ANK: the left half only).
 * Returns 0 when the code has no glyph. */
int pc98gfx_cg_glyph(const Pc98Gfx *g, uint16_t code, uint8_t glyph[32]);

/* A user's own FONT.ROM image (288,768 bytes, the dump format emulators use; read at run time, never shipped):
 * 0000h 256 ANK glyphs of 8x8, 0800h 256 ANK glyphs of 8x16, 1800h kanji: JIS rows 21h-7Ch of 96 cells (20h-7Fh) of 32
 * bytes each (left half, then right half). */
#define PC98GFX_FONTROM_SIZE    288768u
int pc98gfx_fontrom_valid(const uint8_t *rom, size_t size);
const uint8_t *pc98gfx_fontrom_ank16(const uint8_t *rom);
int pc98gfx_fontrom_kanji(void *rom, uint16_t jis, uint8_t glyph[32]);   /* a Pc98GfxKanjiFn; user = the image */

/* ---- for debuggers and tests */
/* A plane of a page as the hardware holds it (no GRCG/EGC). */
uint8_t *pc98gfx_plane(Pc98Gfx *g, int page, int plane);
/* Memory switch n (1-8) as stored, and a way for a BIOS layer to set it whatever flip-flop 1 says. */
uint8_t pc98gfx_memsw(const Pc98Gfx *g, int n);
void pc98gfx_set_memsw(Pc98Gfx *g, int n, uint8_t v);

#endif
