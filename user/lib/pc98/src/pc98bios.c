/*
 * The high-level-emulation BIOS of the pc98 machine. include/pc98bios.h explains the mechanism (stubs in a ROM page of
 * our own, answered in C through the soft_int hook); docs/PC98_BIOS.md lists every call, port and work-area byte with
 * what it returns and where the knowledge comes from: public PC-98 programming references (the BIOS call conventions,
 * the work-area map, the keyboard codes) and observation of the owner's real boot chain with the trace of tools/pc98run.
 * Nothing here comes from a ROM image or from another emulator's source.
 *
 * Conventions inside this file: the guest's registers are read and written in m->cpu; a call that returns a status
 * sets CF in the caller's FLAGS image on the stack (SS:SP+4 when the stub's INT runs: the caller's INT or PUSHF/CALL FAR
 * pushed FLAGS, CS, IP), because the stub's IRET restores FLAGS from there. The BIOS reads and writes guest memory
 * directly (m->ram, pc98_write8) so that its own accesses do not show up in the work-area watch.
 */
#include <pc98.h>
#include <string.h>

typedef X86Cpu C;

#define R(m, i) ((m)->cpu.r[(i)])
#define AX(m) R(m, X86_AX)
#define BX(m) R(m, X86_BX)
#define CX(m) R(m, X86_CX)
#define DX(m) R(m, X86_DX)
#define BP(m) R(m, X86_BP)
#define SI(m) R(m, X86_SI)
#define DI(m) R(m, X86_DI)

static uint8_t ah(Pc98 *m) { return (uint8_t)(AX(m) >> 8); }
static uint8_t al(Pc98 *m) { return (uint8_t)AX(m); }
static void set_ah(Pc98 *m, uint8_t v) { AX(m) = (uint16_t)((AX(m) & 0x00FF) | (uint16_t)v << 8); }
static void set_al(Pc98 *m, uint8_t v) { AX(m) = (uint16_t)((AX(m) & 0xFF00) | v); }
static void set_bh(Pc98 *m, uint8_t v) { BX(m) = (uint16_t)((BX(m) & 0x00FF) | (uint16_t)v << 8); }

static void note(Pc98 *m, int code, uint32_t detail) {
    if (m->event) m->event(m->host, PC98_EV_NOTE, (uint32_t)code, detail);
}

/* ------------------------------------------------------------------ guest memory */

static uint8_t peek(Pc98 *m, uint32_t a) {
    a &= m->cpu.addr_mask;
    return a < PC98_RAM_SIZE ? m->ram[a] : pc98_read8(m, a);
}
static void poke(Pc98 *m, uint32_t a, uint8_t v) { pc98_write8(m, a & m->cpu.addr_mask, v); }
static uint16_t peek16(Pc98 *m, uint32_t a) { return (uint16_t)(peek(m, a) | peek(m, a + 1) << 8); }
static void poke16(Pc98 *m, uint32_t a, uint16_t v) { poke(m, a, (uint8_t)v); poke(m, a + 1, (uint8_t)(v >> 8)); }
static uint32_t lin(Pc98 *m, uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }

/* BIOS work area (always in RAM). */
static uint8_t wa8(Pc98 *m, unsigned off) { return m->ram[off]; }
static uint16_t wa16(Pc98 *m, unsigned off) { return (uint16_t)(m->ram[off] | m->ram[off + 1] << 8); }
static void set_wa8(Pc98 *m, unsigned off, uint8_t v) { m->ram[off] = v; }
static void set_wa16(Pc98 *m, unsigned off, uint16_t v) { m->ram[off] = (uint8_t)v; m->ram[off + 1] = (uint8_t)(v >> 8); }

/* The caller's FLAGS image (the stub's IRET pops it). */
static uint32_t caller_flags_addr(Pc98 *m) {
    return x86_linear(&m->cpu, m->cpu.s[X86_SS], (uint16_t)(R(m, X86_SP) + 4));
}
static void ret_flag(Pc98 *m, uint16_t flag, int on) {
    uint32_t a = caller_flags_addr(m);
    uint16_t f = peek16(m, a);
    f = on ? (uint16_t)(f | flag) : (uint16_t)(f & ~flag);
    poke16(m, a, f);
}
static void ret_cf(Pc98 *m, int on) { ret_flag(m, X86_CF, on); }

/* The call cannot finish yet: run the stub's INT again after the next interrupt, with interrupts enabled meanwhile. */
static void wait_for_interrupt(Pc98 *m) {
    C *c = &m->cpu;
    c->ip = c->op_ip;
    c->flags |= X86_IF;
    c->halted = 1;
}

/* An answer the BIOS does not have: logged loudly, CF set, AH = an error code (callers check CF). */
static void unknown(Pc98 *m, uint8_t vector, uint8_t err) {
    m->bios.unknown++;
    m->bios.last_unknown_vector = vector;
    m->bios.last_unknown_ah = ah(m);
    if (m->event) m->event(m->host, PC98_EV_BIOS_UNKNOWN, vector, ah(m));
    set_ah(m, err);
    ret_cf(m, 1);
}

/* EOI to the master (and the slave for IRQs 8-15), the way an interrupt handler ends. */
static void eoi(Pc98 *m, int irq) {
    if (irq >= 8) pc98_out8(m, 0x08, 0x20);
    pc98_out8(m, 0x00, 0x20);
}

/* ------------------------------------------------------------------ the ROM page */

/* Service stubs (offsets from PC98_BIOS_SVC_STUBS, 4 bytes each). */
enum {
    SVC_BOOT_RETURN = 0,    /* the far call into the boot program returns here */
    SVC_COUNT
};

static void rom_stub(Pc98 *m, unsigned off, uint8_t vector, int iret) {
    m->rom[off] = 0xCD;
    m->rom[off + 1] = vector;
    m->rom[off + 2] = iret ? 0xCF : 0xF4;  /* IRET, or HLT for entries that never return */
    m->rom[off + 3] = 0x90;
}

static void rom_build(Pc98 *m) {
    /* Unused bytes are INT3: execution that lands where no entry point is traps into the BIOS and is reported. */
    memset(m->rom, 0xCC, sizeof m->rom);
    for (unsigned v = 0; v < 256; v++) rom_stub(m, PC98_BIOS_VEC_STUBS + 4 * v, (uint8_t)v, 1);
    for (unsigned s = 0; s < SVC_COUNT; s++) rom_stub(m, PC98_BIOS_SVC_STUBS + 4 * s, 0xFF, 0);
    rom_stub(m, PC98_BIOS_INT1E_ENTRY, 0x1E, 0);
    rom_stub(m, PC98_BIOS_RESET_ENTRY, 0xFF, 0);
    /* A readable identification at the very top, as programs that look at the ROM may expect some text there. */
    static const char id[] = "HLE";
    memcpy(m->rom + PC98_BIOS_ROM_SIZE - 8, id, 3);
}

/* ------------------------------------------------------------------ keyboard */

/* Character codes for the PC-98 key numbers 00h-55h, plain and with SHIFT (0 = no character: the program sees the key
 * number in AH); CTRL turns 40h-7Fh into 00h-1Fh, CAPS swaps the case of letters. Written from the PC-98 keyboard
 * layout (JIS arrangement); GRPH and KANA are not translated (katakana and graphic characters: docs/PC98_BIOS.md). */
static const char kb_plain[0x56] =
    "\x1b" "1234567890-^\\" "\x08" "\x09" "qwertyuiop@[" "\x0d" "asdfghjkl;:]" "zxcvbnm,./_" " "
    "\0" "\0" "\0" "\0" "\x7f" "\x0b" "\x08" "\x0c" "\x0a" "\x1a" "\0"   /* 35 XFER .. 3F HELP */
    "-/789*456+123=0,.";                                                 /* 40-50 keypad */
static const char kb_shift[0x56] =
    "\x1b" "!\"#$%&'()0=`|" "\x08" "\x09" "QWERTYUIOP~{" "\x0d" "ASDFGHJKL+*}" "ZXCVBNM<>?_" " "
    "\0" "\0" "\0" "\0" "\x7f" "\x0b" "\x08" "\x0c" "\x0a" "\x1e" "\0"
    "-/789*456+123=0,.";

static uint8_t kb_char(uint8_t key, uint8_t shift) {
    if (key >= sizeof kb_plain) return 0;
    uint8_t ch = (uint8_t)((shift & 0x01) ? kb_shift[key] : kb_plain[key]);
    if (shift & 0x10) {                         /* CTRL */
        if (ch >= 0x40 && ch < 0x80) return (uint8_t)(ch & 0x1F);
        return ch;
    }
    if ((shift & 0x02) && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 0x20);        /* CAPS */
    else if ((shift & 0x02) && ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + 0x20);
    return ch;
}

int pc98_key_for_char(uint8_t ch, uint8_t *mods) {
    for (unsigned k = 0; k < sizeof kb_plain; k++) {
        if ((uint8_t)kb_plain[k] == ch && ch) { *mods = 0; return (int)k; }
    }
    for (unsigned k = 0; k < sizeof kb_shift; k++) {
        if ((uint8_t)kb_shift[k] == ch && ch) { *mods = 0x01; return (int)k; }
    }
    if (ch < 0x20) {                            /* control characters: CTRL and the letter (or @ [ \ ] ^ _) */
        for (unsigned k = 0; k < sizeof kb_shift; k++) {
            uint8_t c = (uint8_t)kb_shift[k];
            if (c >= 0x40 && c < 0x60 && (c & 0x1F) == ch) { *mods = 0x10; return (int)k; }
        }
        for (unsigned k = 0; k < sizeof kb_plain; k++) {
            uint8_t c = (uint8_t)kb_plain[k];
            if (c >= 0x40 && c < 0x60 && (c & 0x1F) == ch) { *mods = 0x10; return (int)k; }
        }
    }
    return -1;
}

static int kb_fix_pointers(Pc98 *m) {
    uint16_t h = wa16(m, PC98_WA_KB_HEAD), t = wa16(m, PC98_WA_KB_TAIL);
    int bad = (h < PC98_WA_KB_BUF || h >= PC98_WA_KB_BUF_END || (h & 1)) ||
              (t < PC98_WA_KB_BUF || t >= PC98_WA_KB_BUF_END || (t & 1)) || wa8(m, PC98_WA_KB_COUNT) > 16;
    if (bad) {
        set_wa16(m, PC98_WA_KB_HEAD, PC98_WA_KB_BUF);
        set_wa16(m, PC98_WA_KB_TAIL, PC98_WA_KB_BUF);
        set_wa8(m, PC98_WA_KB_COUNT, 0);
    }
    return bad;
}

static void kb_push(Pc98 *m, uint16_t v) {
    kb_fix_pointers(m);
    m->bios.kb_polls = 0;
    if (wa8(m, PC98_WA_KB_COUNT) >= 16) { set_wa8(m, PC98_WA_KB_RETRY, (uint8_t)(wa8(m, PC98_WA_KB_RETRY) + 1)); return; }
    uint16_t t = wa16(m, PC98_WA_KB_TAIL);
    set_wa16(m, t, v);
    t = (uint16_t)(t + 2);
    if (t >= PC98_WA_KB_BUF_END) t = PC98_WA_KB_BUF;
    set_wa16(m, PC98_WA_KB_TAIL, t);
    set_wa8(m, PC98_WA_KB_COUNT, (uint8_t)(wa8(m, PC98_WA_KB_COUNT) + 1));
}

static int kb_peek(Pc98 *m, uint16_t *v) {
    kb_fix_pointers(m);
    if (!wa8(m, PC98_WA_KB_COUNT)) return 0;
    *v = wa16(m, wa16(m, PC98_WA_KB_HEAD));
    return 1;
}

static int kb_pop(Pc98 *m, uint16_t *v) {
    if (!kb_peek(m, v)) return 0;
    uint16_t h = (uint16_t)(wa16(m, PC98_WA_KB_HEAD) + 2);
    if (h >= PC98_WA_KB_BUF_END) h = PC98_WA_KB_BUF;
    set_wa16(m, PC98_WA_KB_HEAD, h);
    set_wa8(m, PC98_WA_KB_COUNT, (uint8_t)(wa8(m, PC98_WA_KB_COUNT) - 1));
    return 1;
}

static void kb_reset(Pc98 *m) {
    set_wa16(m, PC98_WA_KB_HEAD, PC98_WA_KB_BUF);
    set_wa16(m, PC98_WA_KB_TAIL, PC98_WA_KB_BUF);
    set_wa8(m, PC98_WA_KB_COUNT, 0);
    set_wa8(m, PC98_WA_KB_RETRY, 0);
    memset(m->ram + PC98_WA_KB_KEY_STS, 0, 16);
    set_wa8(m, PC98_WA_KB_SHIFT_STS, 0);
}

/* INT 09h, IRQ 1: one byte from the keyboard's 8251 (none ready, as after a stray INT 09h: nothing but the EOI). */
static void int09_keyboard(Pc98 *m) {
    if (!(pc98_in8(m, 0x43) & 0x02)) { eoi(m, 1); return; }    /* RxRDY */
    uint8_t code = pc98_in8(m, 0x41);
    uint8_t key = code & 0x7F, up = code & 0x80;
    uint8_t *sts = &m->ram[PC98_WA_KB_KEY_STS + (key >> 3)];
    uint8_t bit = (uint8_t)(1u << (key & 7));
    if (up) *sts &= (uint8_t)~bit; else *sts |= bit;
    eoi(m, 1);
    if (key >= 0x70 && key <= 0x74) {           /* SHIFT CAPS KANA GRPH CTRL: state only */
        uint8_t s = wa8(m, PC98_WA_KB_SHIFT_STS), b = (uint8_t)(1u << (key - 0x70));
        set_wa8(m, PC98_WA_KB_SHIFT_STS, up ? (uint8_t)(s & ~b) : (uint8_t)(s | b));
        return;
    }
    if (up) return;
    if (key == 0x60) {                          /* STOP: the BIOS raises INT 06h (DOS breaks with it) */
        note(m, PC98_NOTE_STOP_KEY, 0);
        x86_interrupt(&m->cpu, 0x06);
        return;
    }
    kb_push(m, (uint16_t)(key << 8 | kb_char(key, wa8(m, PC98_WA_KB_SHIFT_STS))));
}

/* ------------------------------------------------------------------ INT 18h: keyboard and CRT */

static void gdc_text_on(Pc98 *m, int on) { pc98_out8(m, 0x62, on ? 0x0D : 0x0C); }      /* BCTRL */

static void set_cursor(Pc98 *m, uint16_t addr) {
    m->bios.cursor_addr = addr;
    uint16_t w = (uint16_t)(addr >> 1);
    pc98_out8(m, 0x62, 0x49);                   /* CSRW: the text GDC addresses words */
    pc98_out8(m, 0x60, (uint8_t)w);
    pc98_out8(m, 0x60, (uint8_t)(w >> 8));
    pc98_out8(m, 0x60, 0);
}

/* INT 18h AH=14h: DX = the character (a JIS code, first byte in DH; an ANK code has DH = 0), **BX:CX = the buffer (BX the
 * segment, CX the offset)**: TH01's own wrapper loads them from a far pointer on its stack (`mov bx,[bp+0Ah]; mov cx,[bp+8]`)
 * and reads the buffer back through `les bx,[bp+8]`. (Until 2026-11-03 this took DS:BX, which is what made TH01's menu text
 * come out as the stale bytes of the game's own stack buffer: the "stray marks" of docs/PC98.md.) The buffer gets two
 * header bytes, **the height first, then the width**, in units of 8 dots (2, 1 for a half-width 8x16 character, 2, 2 for a
 * full-width 16x16 one: the games shift both left by 3; TH01's text routine loops over the first as the number of rows and
 * takes the second, == 16 or not, for "two bytes per row" and as the x advance. Until 2026-11-03 the order was width,
 * height: full-width characters were unaffected, half-width ones were drawn as eight rows of two bytes: thin doubled
 * letters), then the pattern row by row, one byte per row for a half-width character and two (left, right) for a
 * full-width one (the game's loop takes the first byte of a pair as the left half). JIS rows 29h-2Bh are NEC's half-width
 * kanji-set characters (their pattern is the left half). **DH = 80h** (DX = 8000h + an ASCII code, how TH01 writes "STAGE01"
 * with a 126-byte stack buffer of seven 18-byte glyphs) is the half-width ANK character DL, answered like DH = 0: 18 bytes. Until
 * 2026-11-03 it was answered as a full-width character, 34 bytes: seven of those ran over the end of the game's buffers, over its
 * saved BP and return address (zeros: the glyph of a missing kanji), the function returned to 0000:0000, the invalid opcode
 * there raised INT 06h and the game's own INT 06h handler (the STOP key's) cleaned up and exit(0)ed: "TH01 quits about 2.5 s
 * after its stage-1 intro". The glyphs come from the font the host gave pc98gfx_set_font(). */
static void font_read(Pc98 *m) {
    uint8_t g[32];
    uint32_t buf = lin(m, BX(m), CX(m));
    uint8_t first = (uint8_t)(DX(m) >> 8), second = (uint8_t)DX(m);
    int ank = first == 0 || first == 0x80;
    int half = ank || (first >= 0x29 && first <= 0x2B);
    memset(g, 0, sizeof g);
    if (ank) pc98gfx_cg_glyph(&m->gfx, second, g);
    else pc98gfx_cg_glyph(&m->gfx, (uint16_t)(second << 8 | (uint8_t)(first - 0x20)), g);
    poke(m, buf, 2);                            /* height: 16 dots */
    poke(m, buf + 1, half ? 1 : 2);             /* width: 8 or 16 dots */
    if (half) {
        for (int i = 0; i < 16; i++) poke(m, buf + 2 + (uint32_t)i, g[i]);
    } else {
        for (int i = 0; i < 16; i++) {
            poke(m, buf + 2 + 2 * (uint32_t)i, g[i]);               /* g: 16 left bytes, then 16 right bytes */
            poke(m, buf + 3 + 2 * (uint32_t)i, g[16 + i]);
        }
    }
}

/* INT 18h AH=1Ah: DX = the user character's JIS code (76xxh or 77xxh), BX:CX = 2 header bytes and the 16x16 pattern in
 * the order AH=14h returns (the segment in BX, the offset in CX: TH01's wrapper for this call loads them the same way).
 * Anything else is ignored (the ROM characters cannot be changed). */
static void font_define(Pc98 *m) {
    uint32_t buf = lin(m, BX(m), CX(m));
    uint8_t first = (uint8_t)(DX(m) >> 8), cell = (uint8_t)DX(m);
    if (first != 0x76 && first != 0x77) return;
    for (int i = 0; i < 16; i++) {
        m->gfx.gaiji[first - 0x76][cell & 0x7F][i] = peek(m, buf + 2 + 2 * (uint32_t)i);
        m->gfx.gaiji[first - 0x76][cell & 0x7F][16 + i] = peek(m, buf + 3 + 2 * (uint32_t)i);
    }
}

static void int18(Pc98 *m) {
    uint16_t key;
    switch (ah(m)) {
    case 0x00:                                  /* read a key, waiting for one */
        if (kb_pop(m, &key)) { AX(m) = key; return; }
        m->bios.key_waits++;
        note(m, PC98_NOTE_KEY_WAIT, 0x00);
        wait_for_interrupt(m);
        return;
    case 0x01:                                  /* sense: BH = 1 and AX = the next key (left in the buffer), or BH = 0 */
        if (kb_peek(m, &key)) { AX(m) = key; set_bh(m, 1); }
        else { set_bh(m, 0); }
        return;
    case 0x02:                                  /* shift state */
        set_al(m, wa8(m, PC98_WA_KB_SHIFT_STS));
        return;
    case 0x03:                                  /* keyboard interface initialisation */
        kb_reset(m);
        return;
    case 0x04:                                  /* key group AL: AH = its 8 bits of the key status table */
        set_ah(m, al(m) < 16 ? wa8(m, PC98_WA_KB_KEY_STS + al(m)) : 0);
        return;
    case 0x05:                                  /* read a key without waiting: BH = 1 and AX = the key, or BH = 0 */
        if (kb_pop(m, &key)) { AX(m) = key; set_bh(m, 1); }
        else { set_bh(m, 0); }
        return;
    case 0x0A:                                  /* CRT mode: bit 0 20 lines, bit 1 40 columns, bit 2 semigraphics, bit 3 KCG dot access */
        m->bios.crt_mode = al(m);
        set_wa8(m, PC98_WA_CRT_STS, al(m));
        return;
    case 0x0B:
        set_al(m, m->bios.crt_mode);
        return;
    case 0x0C: gdc_text_on(m, 1); return;       /* text display on */
    case 0x0D: gdc_text_on(m, 0); return;       /* text display off */
    case 0x0E:                                  /* one display area starting at DX: kept in the GDC's scroll RAM */
        pc98_out8(m, 0x62, 0x70);
        pc98_out8(m, 0x60, (uint8_t)(DX(m) >> 1));
        pc98_out8(m, 0x60, (uint8_t)(DX(m) >> 9));
        return;
    case 0x10:                                  /* cursor blink: AL = 0 blinking, 1 steady */
        m->bios.cursor_blink = al(m) == 0;
        return;
    case 0x11: m->bios.cursor_shown = 1; return;
    case 0x12: m->bios.cursor_shown = 0; return;
    case 0x13: set_cursor(m, DX(m)); return;   /* cursor position: DX = byte offset in text VRAM */
    case 0x16: {                                /* fill text VRAM: DL = character, DH = attribute */
        for (unsigned i = 0; i < 0x2000; i += 2) {
            m->gfx.tvram[i] = (uint8_t)DX(m);
            m->gfx.tvram[i + 1] = 0;
            if (i < 0x1FE0) m->gfx.tvram[0x2000 + i] = (uint8_t)(DX(m) >> 8);   /* the memory switches stay */
        }
        return;
    }
    case 0x14: font_read(m); return;            /* read a character's pattern (the games draw their text with it) */
    case 0x1A: font_define(m); return;          /* define a user character (JIS rows 76h and 77h) */
    case 0x17: pc98_out8(m, 0x37, 0x06); return;    /* beep on (8255 port C bit 3 low) */
    case 0x18: pc98_out8(m, 0x37, 0x07); return;    /* beep off */
    case 0x40: pc98_out8(m, 0xA2, 0x0D); return;    /* graphics display on */
    case 0x41: pc98_out8(m, 0xA2, 0x0C); return;    /* graphics display off */
    case 0x42:
        /* Graphics display mode: CH bits 7-6 = area (11 = all 400 lines, 01/10 = the upper/lower 200-line half),
         * bit 5 = monochrome, bit 4 = the 200-line page. Recorded; the frame is always rendered as 640x400 here
         * (the graphics renderer of the gfx module may use it). */
        m->bios.gfx_mode = (uint8_t)(CX(m) >> 8);
        return;
    default:
        unknown(m, 0x18, 0);
        return;
    }
}

/* ------------------------------------------------------------------ INT 1Bh: disks */

/* Status codes (AH; CF set when not 00h). */
enum {
    DSK_OK = 0x00, DSK_WRITE_PROTECT = 0x70, DSK_NOT_READY = 0x60, DSK_EQUIPMENT = 0x40, DSK_NO_DATA = 0xC0,
    DSK_BAD_CMD = 0x40,
};

typedef struct {
    Pc98BiosUnit *u;
    int hd;             /* hard disk (else floppy) */
    int relative;       /* hard disk addressed by relative sector number (DA/UA bit 7 clear) */
} Drive;

static int find_drive(Pc98 *m, uint8_t daua, Drive *d) {
    memset(d, 0, sizeof *d);
    uint8_t unit = daua & 0x0F;
    switch (daua & 0xF0) {
    case 0x80: case 0x00:
        if (unit >= 4) return -1;
        d->u = &m->bios.hd[unit];
        d->hd = 1;
        d->relative = !(daua & 0x80);
        break;
    case 0x90:
        if (unit >= 4) return -1;
        d->u = &m->bios.fd[unit];
        break;
    default:
        return -1;                              /* SCSI (A0h), 640 KB drives (70h), 1.44 MB mode (30h): none */
    }
    return d->u->disk ? 0 : -1;
}

/* Copies one sector between the image and guest memory at `addr`, at most `n` bytes. Returns the bytes moved. */
static uint32_t xfer(Pc98 *m, Drive *d, const Pc98Sector *s, uint32_t addr, uint32_t n, int write, int *wp) {
    if (n > s->size) n = s->size;
    if (write) {
        if (!d->u->writable) { *wp = 1; return 0; }
        uint8_t *dst = d->u->writable + (s->data - d->u->disk->data);
        for (uint32_t i = 0; i < n; i++) dst[i] = peek(m, addr + i);
        m->bios.disk_writes++;
    } else {
        for (uint32_t i = 0; i < n; i++) poke(m, addr + i, s->data[i]);
        m->bios.disk_reads++;
    }
    return n;
}

/* Read, write or verify BX bytes (0 = 64 KiB) at the address in CX/DX for drive d, buffer ES:BP. Hard disks: CX =
 * cylinder, DH = head, DL = sector (from 0), or (relative addressing) DX:CX = sector number; the transfer runs on
 * through the following sectors, heads and cylinders. Floppies: CL = cylinder, DH = head, DL = sector (from 1), CH =
 * sector size code; the transfer runs on to the next sector, the other head, then the next cylinder. */
static uint8_t disk_rw(Pc98 *m, Drive *d, int write, int verify) {
    const Pc98Disk *disk = d->u->disk;
    uint32_t left = BX(m) ? BX(m) : 0x10000u;
    uint32_t addr = lin(m, m->cpu.s[X86_ES], BP(m));
    uint32_t cyl, head, sec;
    if (!disk->heads || !disk->sectors) return DSK_NO_DATA;
    if (d->hd) {
        if (d->relative) {
            uint32_t lba = (uint32_t)DX(m) << 16 | CX(m), per_cyl = disk->heads * disk->sectors;
            cyl = lba / per_cyl;
            head = (lba % per_cyl) / disk->sectors;
            sec = lba % disk->sectors;
        } else {
            cyl = CX(m);
            head = (uint8_t)(DX(m) >> 8);
            sec = (uint8_t)DX(m);
        }
        sec += 1;                               /* the image numbers sectors from 1 */
    } else {
        cyl = (uint8_t)CX(m);
        head = (uint8_t)(DX(m) >> 8);
        sec = (uint8_t)DX(m);
    }
    int wp = 0;
    while (left) {
        Pc98Sector s;
        if (pc98_disk_sector(disk, cyl, head, sec, &s) != PC98_DISK_OK || !s.size) {
            note(m, PC98_NOTE_DISK_RANGE, al(m));
            return DSK_NO_DATA;
        }
        uint32_t n = verify ? (left < s.size ? left : s.size) : xfer(m, d, &s, addr, left, write, &wp);
        if (wp) return DSK_WRITE_PROTECT;
        left -= n;
        addr += n;
        /* the next sector */
        if (d->hd) {
            if (++sec > disk->sectors) { sec = 1; if (++head >= disk->heads) { head = 0; cyl++; } }
        } else {
            sec++;
            if (pc98_disk_sector(disk, cyl, head, sec, &s) != PC98_DISK_OK) {
                sec = 1;
                if (++head >= 2) { head = 0; cyl++; }
            }
        }
    }
    return DSK_OK;
}

static void int1b(Pc98 *m) {
    Drive d;
    uint8_t cmd = ah(m) & 0x0F, status;
    if (find_drive(m, al(m), &d) < 0) {         /* no such unit */
        set_ah(m, DSK_NOT_READY);
        ret_cf(m, 1);
        return;
    }
    const Pc98Disk *disk = d.u->disk;
    switch (cmd) {
    case 0x01: status = disk_rw(m, &d, 0, 1); break;                    /* verify */
    case 0x03: status = DSK_OK; break;                                  /* initialise */
    case 0x04:                                                          /* sense */
        if (d.hd && (ah(m) & 0x80)) {           /* "new sense" (84h): the geometry */
            BX(m) = (uint16_t)disk->sector_size;
            CX(m) = (uint16_t)disk->cylinders;
            DX(m) = (uint16_t)(disk->heads << 8 | disk->sectors);
        }
        status = (!d.hd && d.u->disk->write_protect) ? 0x10 : DSK_OK;
        break;
    case 0x05: status = disk_rw(m, &d, 1, 0); break;                    /* write data */
    case 0x06: status = disk_rw(m, &d, 0, 0); break;                    /* read data */
    case 0x07: status = DSK_OK; break;                                  /* recalibrate (seek to cylinder 0) */
    case 0x0E:
        /* Hard disk 8Eh: the IPL, NEC's boot menu and IO.SYS call it once per drive before they use a disk with
         * 512-byte sectors, and none of them looks at the answer (docs/PC98_BIOS.md). An image has one fixed
         * sector size, so there is no mode to set: success. Floppy 0Eh is another call: not implemented. */
        if (!d.hd || !(ah(m) & 0x80)) { unknown(m, 0x1B, DSK_BAD_CMD); return; }
        status = DSK_OK;
        break;
    case 0x0F:
        /* Hard disk 0Fh: the STOP-key handlers of IO.SYS (MS-DOS 3.30C and 6.20) call it for every hard disk unit
         * and ignore the answer. An HLE disk has no operation in flight to give up: success, nothing else. */
        if (!d.hd) { unknown(m, 0x1B, DSK_BAD_CMD); return; }
        status = DSK_OK;
        break;
    default:
        unknown(m, 0x1B, DSK_BAD_CMD);
        return;
    }
    set_ah(m, status);
    ret_cf(m, status != DSK_OK && status != 0x10);
}

/* ------------------------------------------------------------------ INT 1Ch: calendar and interval timer */

static unsigned from_bcd(uint8_t v) { return (unsigned)(v >> 4) * 10u + (v & 15u); }
static uint8_t to_bcd(unsigned v) { return (uint8_t)((v / 10u) << 4 | (v % 10u)); }

static unsigned days_in_month(unsigned year, unsigned month) {
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && year % 4 == 0) return 29;     /* 2000-2099: every fourth year */
    return month >= 1 && month <= 12 ? d[month - 1] : 31;
}

/* 0 Sunday .. 6 Saturday, for 2000-2099 (2000-01-01 was a Saturday). */
static unsigned weekday(unsigned year, unsigned month, unsigned day) {
    unsigned days = 0;
    for (unsigned y = 0; y < year; y++) days += (y % 4 == 0) ? 366 : 365;
    for (unsigned mo = 1; mo < month; mo++) days += days_in_month(year, mo);
    days += day - 1;
    return (days + 6) % 7;
}

void pc98_bios_set_clock(Pc98 *m, const uint8_t bcd[6]) {
    memcpy(m->bios.clock_bcd, bcd, 6);
    unsigned y = from_bcd(bcd[0]), mo = bcd[1] >> 4, d = from_bcd(bcd[2]);
    m->bios.clock_bcd[1] = (uint8_t)(mo << 4 | weekday(y, mo, d));
    m->bios.clock_cycles = m->cpu.cycles;
}

void pc98_bios_clock_now(Pc98 *m, uint8_t out[6]) {
    const uint8_t *b = m->bios.clock_bcd;
    unsigned y = from_bcd(b[0]), mo = b[1] >> 4, d = from_bcd(b[2]);
    uint64_t secs = (uint64_t)from_bcd(b[3]) * 3600u + from_bcd(b[4]) * 60u + from_bcd(b[5]);
    secs += (m->cpu.cycles - m->bios.clock_cycles) / (m->cpu_hz ? m->cpu_hz : 1);
    while (secs >= 86400u) {
        secs -= 86400u;
        if (++d > days_in_month(y, mo)) { d = 1; if (++mo > 12) { mo = 1; y = (y + 1) % 100; } }
    }
    out[0] = to_bcd(y);
    out[1] = (uint8_t)(mo << 4 | weekday(y, mo, d));
    out[2] = to_bcd(d);
    out[3] = to_bcd((unsigned)(secs / 3600u));
    out[4] = to_bcd((unsigned)(secs / 60u % 60u));
    out[5] = to_bcd((unsigned)(secs % 60u));
}

static void timer_program(Pc98 *m) {
    uint16_t count = (uint16_t)(m->pit_hz / 100u);      /* 10 ms */
    pc98_out8(m, 0x77, 0x34);                           /* counter 0, LSB then MSB, mode 2 */
    pc98_out8(m, 0x71, (uint8_t)count);
    pc98_out8(m, 0x71, (uint8_t)(count >> 8));
    pc98_out8(m, 0x02, (uint8_t)(pc98_in8(m, 0x02) & ~0x01));   /* unmask IRQ 0 */
}

static void int1c(Pc98 *m) {
    uint32_t buf = lin(m, m->cpu.s[X86_ES], BX(m));
    uint8_t t[6];
    switch (ah(m)) {
    case 0x00:                                  /* read the calendar to ES:BX (6 BCD bytes) */
        pc98_bios_clock_now(m, t);
        for (int i = 0; i < 6; i++) poke(m, buf + (uint32_t)i, t[i]);
        return;
    case 0x01:                                  /* set the calendar from ES:BX */
        for (int i = 0; i < 6; i++) t[i] = peek(m, buf + (uint32_t)i);
        pc98_bios_set_clock(m, t);
        return;
    case 0x02:                                  /* interval timer: CX ticks of 10 ms, then INT 07h's vector := ES:BX is called */
        poke16(m, 0x07 * 4, BX(m));
        poke16(m, 0x07 * 4 + 2, m->cpu.s[X86_ES]);
        m->bios.timer_ticks = CX(m);
        m->bios.timer_active = 1;
        timer_program(m);
        return;
    default:
        unknown(m, 0x1C, 0);
        return;
    }
}

/* INT 08h, IRQ 0: the interval timer's tick. */
static void int08_timer(Pc98 *m) {
    eoi(m, 0);
    if (!m->bios.timer_active) return;
    if (m->bios.timer_ticks > 1) { m->bios.timer_ticks--; return; }
    m->bios.timer_active = 0;
    pc98_out8(m, 0x02, (uint8_t)(pc98_in8(m, 0x02) | 0x01));       /* mask IRQ 0 again */
    x86_interrupt(&m->cpu, 0x07);              /* the user's routine; its IRET comes back to the stub's IRET */
}

/* ------------------------------------------------------------------ the boot */

static void svc(Pc98 *m, unsigned id) {
    C *c = &m->cpu;
    switch (id) {
    case SVC_BOOT_RETURN:                       /* the boot program gave up (RETF to the BIOS) */
        note(m, PC98_NOTE_BOOT_RETURNED, CX(m));
        c->flags &= (uint16_t)~X86_IF;
        c->halted = 1;
        c->stop = PC98_STOP_BOOT_FAILED;
        c->ip = c->op_ip;
        return;
    }
}

int pc98_bios_boot(Pc98 *m, uint8_t da_ua) {
    Drive d;
    if (!m->bios.enabled || find_drive(m, da_ua, &d) < 0) return -1;
    Pc98Sector s;                               /* the first sector: C0 H0 R1 in the image's numbering */
    if (pc98_disk_sector(d.u->disk, 0, 0, 1, &s) != PC98_DISK_OK || !s.size) return -1;
    uint32_t n = s.size > 0x2000 ? 0x2000 : s.size;
    memcpy(m->ram + ((uint32_t)PC98_IPL_SEG << 4), s.data, n);
    m->bios.boot_device = da_ua;
    set_wa8(m, PC98_WA_DISK_BOOT, da_ua);
    C *c = &m->cpu;
    memset(c->r, 0, sizeof c->r);
    c->r[X86_AX] = da_ua;
    c->s[X86_DS] = 0;
    c->s[X86_ES] = 0;
    c->s[X86_SS] = PC98_BIOS_STACK_SEG;
    c->r[X86_SP] = PC98_BIOS_STACK_TOP;
    /* the far call: return address = the BIOS's boot-return stub */
    c->r[X86_SP] = (uint16_t)(c->r[X86_SP] - 4);
    uint32_t sp = lin(m, c->s[X86_SS], c->r[X86_SP]);
    poke16(m, sp, (uint16_t)(PC98_BIOS_SVC_STUBS + 4 * SVC_BOOT_RETURN));
    poke16(m, sp + 2, PC98_BIOS_SEG);
    c->s[X86_CS] = PC98_IPL_SEG;
    c->ip = 0;
    c->flags &= (uint16_t)~(X86_IF | X86_TF | X86_DF);
    c->halted = 0;
    c->stop = 0;
    note(m, PC98_NOTE_BOOT_LOADED, da_ua);
    return 0;
}

/* ------------------------------------------------------------------ dispatch */

/* CPU exceptions and the other low vectors with nothing behind them: report, return. The same fault at the same
 * place many times in a row (a 286 fault returns to the faulting instruction) stops the CPU. */
static void exception(Pc98 *m, uint8_t vector) {
    uint32_t ret = lin(m, peek16(m, lin(m, m->cpu.s[X86_SS], (uint16_t)(R(m, X86_SP) + 2))),
                       peek16(m, lin(m, m->cpu.s[X86_SS], R(m, X86_SP))));
    uint32_t key = ret << 8 | vector;
    if (key == m->bios.fault_last) {
        if (++m->bios.fault_repeats >= 1000) { m->cpu.stop = PC98_STOP_FAULT_LOOP; m->cpu.ip = m->cpu.op_ip; }
    } else {
        m->bios.fault_last = key;
        m->bios.fault_repeats = 0;
        note(m, PC98_NOTE_EXCEPTION, vector);
    }
}

static int vector_call(Pc98 *m, uint8_t v) {
    switch (v) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        exception(m, v);
        return 0;
    case 0x08: int08_timer(m); return 0;
    case 0x09: int09_keyboard(m); return 0;
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        eoi(m, v - 0x08);
        return 0;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        eoi(m, v - 0x10 + 8);
        return 0;
    case 0x18: int18(m); return m->cpu.halted;
    case 0x1B: int1b(m); return 0;
    case 0x1C: int1c(m); return 0;
    case 0x19: case 0x1A: case 0x1F:            /* RS-232C, printer, extended memory: not present */
        unknown(m, v, 0);
        return 0;
    default:                                    /* vectors nobody installed: report, return */
        unknown(m, v, ah(m));
        return 0;
    }
}

int pc98_bios_soft_int(Pc98 *m, uint8_t vector) {
    C *c = &m->cpu;
    uint32_t at = lin(m, c->s[X86_CS], c->op_ip);
    if (at < PC98_BIOS_ROM_BASE || at >= PC98_BIOS_ROM_BASE + PC98_BIOS_ROM_SIZE) return 0;
    uint32_t off = at - PC98_BIOS_ROM_BASE;
    m->bios.calls++;
    if (off < PC98_BIOS_SVC_STUBS && (off & 3) == 0 && m->rom[off] == 0xCD) {
        uint8_t v = (uint8_t)(off / 4);
        if (m->event) m->event(m->host, PC98_EV_BIOS_CALL, v, 0);
        int waiting = vector_call(m, v);
        if (m->event) m->event(m->host, PC98_EV_BIOS_DONE, v, (uint32_t)waiting);
        return 1;
    }
    if (off >= PC98_BIOS_SVC_STUBS && off < PC98_BIOS_SVC_STUBS + 4 * SVC_COUNT && (off & 3) == 0) {
        svc(m, (off - PC98_BIOS_SVC_STUBS) / 4);
        return 1;
    }
    if (off == PC98_BIOS_INT1E_ENTRY || off == PC98_BIOS_RESET_ENTRY) {
        note(m, PC98_NOTE_RESET, off);
        c->stop = PC98_STOP_RESET;
        c->ip = c->op_ip;
        return 1;
    }
    /* An INT3 from the filler (or any INT) somewhere else in the page: a call to an entry point we do not have. */
    note(m, PC98_NOTE_ROM_ENTRY, off);
    c->stop = PC98_STOP_ROM_ENTRY;
    c->ip = c->op_ip;
    (void)vector;
    return 1;
}

/* ------------------------------------------------------------------ set-up */

static void pic_init(Pc98 *m) {
    static const uint8_t master[] = { 0x11, 0x08, 0x80, 0x1D }, slave[] = { 0x11, 0x10, 0x07, 0x09 };
    pc98_out8(m, 0x00, master[0]);
    for (int i = 1; i < 4; i++) pc98_out8(m, 0x02, master[i]);
    pc98_out8(m, 0x08, slave[0]);
    for (int i = 1; i < 4; i++) pc98_out8(m, 0x0A, slave[i]);
    pc98_out8(m, 0x02, 0x7D);                   /* master: keyboard (IR1) and the slave (IR7) open */
    pc98_out8(m, 0x0A, 0xFF);                   /* slave: all closed (the HLE disks need no interrupt) */
}

static void update_equipment(Pc98 *m) {
    uint8_t fd = 0, hd = 0;
    for (int i = 0; i < 4; i++) {
        if (m->bios.fd[i].disk) fd |= (uint8_t)(1u << i);
        if (m->bios.hd[i].disk) hd |= (uint8_t)(1u << i);
    }
    set_wa8(m, PC98_WA_DISK_EQUIP, (uint8_t)((wa8(m, PC98_WA_DISK_EQUIP) & 0xF0) | fd));
    set_wa8(m, PC98_WA_DISK_EQUIP + 1, (uint8_t)((wa8(m, PC98_WA_DISK_EQUIP + 1) & 0xF0) | hd));
}

int pc98_bios_attach(Pc98 *m, uint8_t da_ua, const Pc98Disk *disk, uint8_t *writable) {
    uint8_t unit = da_ua & 0x0F;
    if (unit >= 4) return -1;
    Pc98BiosUnit *u;
    if ((da_ua & 0xF0) == 0x80) u = &m->bios.hd[unit];
    else if ((da_ua & 0xF0) == 0x90) u = &m->bios.fd[unit];
    else return -1;
    u->disk = disk;
    u->writable = disk ? writable : NULL;
    update_equipment(m);
    return 0;
}

/* Memory switches as a machine leaves the factory (the values the SWITCH utility calls standard). */
static const uint8_t memsw_default[8] = { 0x48, 0x05, 0x04, 0x00, 0x01, 0x00, 0x00, 0x6E };

void pc98_bios_init(Pc98 *m) {
    Pc98BiosUnit hd[4], fd[4];
    memcpy(hd, m->bios.hd, sizeof hd);          /* drives attached before a re-init stay */
    memcpy(fd, m->bios.fd, sizeof fd);
    memset(&m->bios, 0, sizeof m->bios);
    memcpy(m->bios.hd, hd, sizeof hd);
    memcpy(m->bios.fd, fd, sizeof fd);
    rom_build(m);
    m->bios.enabled = 1;

    /* Interrupt vectors: every one points at its stub in the ROM page. */
    for (unsigned v = 0; v < 256; v++) {
        poke16(m, v * 4, (uint16_t)(PC98_BIOS_VEC_STUBS + 4 * v));
        poke16(m, v * 4 + 2, PC98_BIOS_SEG);
    }

    /* Work area. */
    memset(m->ram + 0x400, 0, 0x200);
    uint8_t flag1 = 0x04;                       /* 640 KiB: (4 + 1) x 128 KiB */
    flag1 |= 0x20;                              /* model bits 4-5 = 10: a machine with the newer BIOS (docs/PC98_BIOS.md) */
    if (m->pit_hz == 1996800) flag1 |= 0x80;    /* the 8 MHz family: timer clock 1.9968 MHz */
    set_wa8(m, PC98_WA_BIOS_FLAG1, flag1);
    set_wa8(m, PC98_WA_EXPMMSZ, 0);
    set_wa8(m, PC98_WA_CRT_ROW, 0x0F);          /* 16 rasters per text row, minus 1: TH01 builds its CSRFORM from this byte */
    kb_reset(m);
    update_equipment(m);

    /* Memory switches in the text attribute RAM. */
    for (unsigned i = 0; i < 8; i++) pc98gfx_set_memsw(&m->gfx, (int)i + 1, memsw_default[i]);

    /* Devices as the BIOS leaves them. */
    pic_init(m);
    pc98_out8(m, 0x77, 0x76);                   /* timer counter 1 (beeper): mode 3, about 2 kHz */
    uint16_t beep = (uint16_t)(m->pit_hz / 2000u);
    pc98_out8(m, 0x73, (uint8_t)beep);
    pc98_out8(m, 0x73, (uint8_t)(beep >> 8));
    pc98_out8(m, 0x37, 0x07);                   /* beeper off */
    pc98_out8(m, 0x43, 0x5E);                   /* keyboard 8251: the mode word, then RxE, DTR and error reset */
    pc98_out8(m, 0x43, 0x16);
    gdc_text_on(m, 1);
    m->bios.cursor_shown = 1;
    m->bios.cursor_blink = 1;
    set_cursor(m, 0);
    static const uint8_t epoch[6] = { 0x26, 0x11, 0x02, 0x12, 0x00, 0x00 };   /* 2026-11-02 12:00:00 until set */
    pc98_bios_set_clock(m, epoch);
}

/* ------------------------------------------------------------------ names */

const char *pc98_bios_note_name(int code) {
    switch (code) {
    case PC98_NOTE_BOOT_LOADED: return "boot sector loaded";
    case PC98_NOTE_BOOT_RETURNED: return "boot program returned to the BIOS";
    case PC98_NOTE_ROM_ENTRY: return "call to an unknown ROM entry point";
    case PC98_NOTE_EXCEPTION: return "CPU exception";
    case PC98_NOTE_KEY_WAIT: return "waiting for a key";
    case PC98_NOTE_DISK_RANGE: return "disk sector out of range";
    case PC98_NOTE_STOP_KEY: return "STOP key";
    case PC98_NOTE_RESET: return "reboot requested";
    }
    return "?";
}

const char *pc98_stop_name(int code) {
    switch (code) {
    case X86_STOP_NONE: return "running";
    case X86_STOP_PROTECTED: return "protected mode requested";
    case X86_STOP_PREFIXES: return "endless prefixes";
    case X86_STOP_UNIMPLEMENTED: return "unimplemented instruction";
    case PC98_STOP_BOOT_FAILED: return "boot failed (the boot program returned)";
    case PC98_STOP_ROM_ENTRY: return "unknown ROM entry point";
    case PC98_STOP_FAULT_LOOP: return "the same CPU exception over and over";
    case PC98_STOP_RESET: return "reboot requested";
    }
    return "?";
}
