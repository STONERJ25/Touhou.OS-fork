/*
 * The PC-98 machine skeleton: memory map, I/O port dispatcher, device timing, boot hand-over, text dump and a renderer.
 * See include/pc98.h and docs/PC98.md (the port table there lists what each port does here).
 */
#include <pc98.h>
#include <string.h>

/* ------------------------------------------------------------------ memory */

uint8_t pc98_read8(Pc98 *m, uint32_t a) {
    if (a < PC98_RAM_SIZE) return m->ram[a];
    if (a < PC98_TVRAM_END) return m->tvram[a - PC98_TVRAM];
    if (a >= PC98_GVRAM_B && a < 0xC0000) return m->gvram[m->draw_page][(a - PC98_GVRAM_B) >> 15][a & 0x7FFF];
    if (a >= PC98_GVRAM_I && a < PC98_ROM) return m->gvram[m->draw_page][3][a & 0x7FFF];
    return 0xFF;                                /* CG window, expansion area, the empty ROM, beyond 1 MiB */
}

void pc98_write8(Pc98 *m, uint32_t a, uint8_t v) {
    if (a < PC98_RAM_SIZE) m->ram[a] = v;
    else if (a < PC98_TVRAM_END) m->tvram[a - PC98_TVRAM] = v;
    else if (a >= PC98_GVRAM_B && a < 0xC0000) m->gvram[m->draw_page][(a - PC98_GVRAM_B) >> 15][a & 0x7FFF] = v;
    else if (a >= PC98_GVRAM_I && a < PC98_ROM) m->gvram[m->draw_page][3][a & 0x7FFF] = v;
}

/* ------------------------------------------------------------------ interrupts */

static void update_cascade(Pc98 *m) {
    pc98_pic_set_line(&m->pic[0], 7, pc98_pic_pending(&m->pic[1]) >= 0);
}

void pc98_irq(Pc98 *m, int irq, int level) {
    if (irq < 8) pc98_pic_set_line(&m->pic[0], irq, level);
    else pc98_pic_set_line(&m->pic[1], irq - 8, level);
    update_cascade(m);
}

int pc98_intr_ack(Pc98 *m) {
    update_cascade(m);
    int i = pc98_pic_pending(&m->pic[0]);
    if (i < 0) return -1;
    if (i == 7 && (m->pic[0].icw3 & 0x80)) {    /* the slave hangs on master IR7 */
        pc98_pic_ack(&m->pic[0]);
        int s = pc98_pic_ack(&m->pic[1]);
        update_cascade(m);
        return (m->pic[1].icw2 & 0xF8) | (s < 0 ? 7 : s);  /* nothing there: the slave's spurious IR7 */
    }
    pc98_pic_ack(&m->pic[0]);
    return (m->pic[0].icw2 & 0xF8) | i;
}

/* ------------------------------------------------------------------ keyboard */

static void kbd_feed(Pc98 *m) {
    Pc98Kbd *k = &m->kbd;
    if (k->rxrdy || !k->qlen) return;
    k->data = k->queue[k->qhead];
    k->qhead = (uint8_t)((k->qhead + 1) % sizeof k->queue);
    k->qlen--;
    k->rxrdy = 1;
    pc98_irq(m, 1, 1);
}

void pc98_key(Pc98 *m, uint8_t scancode, int down) {
    Pc98Kbd *k = &m->kbd;
    if (k->qlen == sizeof k->queue) { k->overrun = 1; return; }
    k->queue[(k->qhead + k->qlen) % sizeof k->queue] = (uint8_t)((scancode & 0x7F) | (down ? 0 : 0x80));
    k->qlen++;
    kbd_feed(m);
}

/* ------------------------------------------------------------------ I/O ports */

static void set_beeper(Pc98 *m) {
    uint8_t on = !(m->sys_c & 0x08);            /* port C bit 3 low = buzzer on */
    if (on != m->beep_on) { m->beep_on = on; m->beep_changes++; }
}

uint32_t pc98_beep_hz(const Pc98 *m) {
    if (!m->pit[1].loaded || !m->pit[1].reload) return 0;
    return m->pit_hz / m->pit[1].reload;
}

static uint8_t unhandled_in(Pc98 *m, uint16_t port) {
    if (m->port_in) {
        int v = m->port_in(m->host, port);
        if (v >= 0) return (uint8_t)v;
    }
    m->unhandled++;
    m->last_unhandled_port = port;
    return 0xFF;
}

uint8_t pc98_in8(Pc98 *m, uint16_t port) {
    switch (port) {
    case 0x00: case 0x02: return pc98_pic_read(&m->pic[0], port == 0x02);
    case 0x08: case 0x0A: return pc98_pic_read(&m->pic[1], port == 0x0A);
    case 0x31: return m->sys_a;
    case 0x33: return m->sys_b;
    case 0x35: return m->sys_c;
    case 0x41: {
        uint8_t v = m->kbd.data;
        m->kbd.rxrdy = 0;
        pc98_irq(m, 1, 0);
        kbd_feed(m);
        return v;
    }
    case 0x43: return (uint8_t)(0x85 | (m->kbd.rxrdy ? 0x02 : 0) | (m->kbd.overrun ? 0x10 : 0));
    case 0x60: return pc98_gdc_status(&m->gdc[0], m->vsync);
    case 0x62: return pc98_gdc_read_data(&m->gdc[0]);
    case 0xA0: return pc98_gdc_status(&m->gdc[1], m->vsync);
    case 0xA2: return pc98_gdc_read_data(&m->gdc[1]);
    case 0x71: case 0x3FD9: return pc98_pit_read(&m->pit[0]);
    case 0x73: case 0x3FDB: return pc98_pit_read(&m->pit[1]);
    case 0x75: case 0x3FDD: return pc98_pit_read(&m->pit[2]);
    case 0xA4: return m->disp_page;
    case 0xA6: return m->draw_page;
    case 0xA8: return m->mode_ff2_analog ? m->pal_index : m->pal_digital[0];
    case 0xAA: return m->mode_ff2_analog ? m->pal_analog[m->pal_index & 15][0] : m->pal_digital[1];
    case 0xAC: return m->mode_ff2_analog ? m->pal_analog[m->pal_index & 15][1] : m->pal_digital[2];
    case 0xAE: return m->mode_ff2_analog ? m->pal_analog[m->pal_index & 15][2] : m->pal_digital[3];
    case 0xF2: return m->a20 ? 0x00 : 0x01;     /* bit 0: A20 masked */
    default: return unhandled_in(m, port);
    }
}

static void a20_set(Pc98 *m, int on) {
    m->a20 = (uint8_t)on;
    m->cpu.addr_mask = (on && m->cpu.model == X86_MODEL_80286) ? 0xFFFFFF : 0xFFFFF;
}

void pc98_out8(Pc98 *m, uint16_t port, uint8_t v) {
    switch (port) {
    case 0x00: case 0x02: pc98_pic_write(&m->pic[0], port == 0x02, v); update_cascade(m); return;
    case 0x08: case 0x0A: pc98_pic_write(&m->pic[1], port == 0x0A, v); update_cascade(m); return;
    case 0x35: m->sys_c = v; set_beeper(m); return;
    case 0x37:
        if (!(v & 0x80)) {                      /* bit set/reset on port C */
            uint8_t bit = (uint8_t)(1 << ((v >> 1) & 7));
            if (v & 1) m->sys_c |= bit; else m->sys_c &= (uint8_t)~bit;
            set_beeper(m);
        }
        return;                                 /* mode words: the system port's mode is fixed */
    case 0x41: m->kbd.to_keyboard++; return;
    case 0x43:
        if (m->kbd.mode_next) { m->kbd.mode = v; m->kbd.mode_next = 0; return; }
        m->kbd.command = v;
        if (v & 0x10) m->kbd.overrun = 0;       /* error reset */
        if (v & 0x40) m->kbd.mode_next = 1;     /* internal reset */
        return;
    case 0x50: m->nmi_enabled = 0; return;
    case 0x52: m->nmi_enabled = 1; return;
    case 0x5F: return;                          /* the 0.6 us wait port */
    case 0x60: pc98_gdc_param(&m->gdc[0], v); return;
    case 0x62: pc98_gdc_command(&m->gdc[0], v); return;
    case 0x64: m->vsync_irq_armed = 1; return;  /* CRT interrupt reset: arms the next VSYNC interrupt */
    case 0x68: {                                /* mode flip-flop 1: 0000nnnv */
        uint8_t bit = (uint8_t)(1 << ((v >> 1) & 7));
        if (v & 1) m->mode_ff1 |= bit; else m->mode_ff1 &= (uint8_t)~bit;
        return;
    }
    case 0x6A:                                  /* mode flip-flop 2: 00h digital, 01h analog (16 colours) */
        if ((v & 0xFE) == 0x00) m->mode_ff2_analog = v & 1;
        return;
    case 0x6C: m->border = v; return;
    case 0x70: case 0x72: case 0x74: case 0x76: case 0x78: case 0x7A: return;  /* text CRTC: line timing, ignored */
    case 0x71: case 0x3FD9: pc98_pit_write(&m->pit[0], v); return;
    case 0x73: case 0x3FDB: pc98_pit_write(&m->pit[1], v); return;
    case 0x75: case 0x3FDD: pc98_pit_write(&m->pit[2], v); return;
    case 0x77: case 0x3FDF: pc98_pit_control(m->pit, v); return;
    case 0xA0: pc98_gdc_param(&m->gdc[1], v); return;
    case 0xA2: pc98_gdc_command(&m->gdc[1], v); return;
    case 0xA4: m->disp_page = v & 1; return;
    case 0xA6: m->draw_page = v & 1; return;
    case 0xA8: if (m->mode_ff2_analog) m->pal_index = v & 15; else m->pal_digital[0] = v; return;
    case 0xAA: if (m->mode_ff2_analog) m->pal_analog[m->pal_index][0] = v & 15; else m->pal_digital[1] = v; return;
    case 0xAC: if (m->mode_ff2_analog) m->pal_analog[m->pal_index][1] = v & 15; else m->pal_digital[2] = v; return;
    case 0xAE: if (m->mode_ff2_analog) m->pal_analog[m->pal_index][2] = v & 15; else m->pal_digital[3] = v; return;
    case 0xF0: m->cpu_resets++; return;         /* CPU reset: needs the BIOS's resume path, not modelled */
    case 0xF2: a20_set(m, 1); return;
    case 0xF6:
        if (v == 0x02) a20_set(m, 1);
        else if (v == 0x03) a20_set(m, 0);
        return;
    default:
        if (m->port_out && m->port_out(m->host, port, v) >= 0) return;
        m->unhandled++;
        m->last_unhandled_port = port;
        return;
    }
}

/* ------------------------------------------------------------------ CPU bus glue */

static uint8_t bus_read(void *ctx, uint32_t a) { return pc98_read8((Pc98 *)ctx, a); }
static void bus_write(void *ctx, uint32_t a, uint8_t v) { pc98_write8((Pc98 *)ctx, a, v); }
static uint8_t bus_in(void *ctx, uint16_t p) { return pc98_in8((Pc98 *)ctx, p); }
static void bus_out(void *ctx, uint16_t p, uint8_t v) { pc98_out8((Pc98 *)ctx, p, v); }
static int bus_intr(void *ctx) { return pc98_intr_ack((Pc98 *)ctx); }

/* ------------------------------------------------------------------ time */

void pc98_advance(Pc98 *m, uint64_t cyc) {
    if (!cyc) return;
    /* Interval timer: its own clock, carried as a remainder so nothing drifts. */
    m->pit_frac += cyc * m->pit_hz;
    uint64_t ticks = m->pit_frac / m->cpu_hz;
    m->pit_frac %= m->cpu_hz;
    if (ticks) {
        uint64_t e0 = m->pit[0].edges;
        for (int i = 0; i < 3; i++) pc98_pit_advance(&m->pit[i], ticks);
        if (m->pit[0].edges != e0) {            /* OUT0 rose: an edge on IR0 */
            pc98_irq(m, 0, 1);
            pc98_irq(m, 0, 0);
        }
    }
    /* Video timing: VSYNC at the start of each frame. */
    uint64_t pos = m->frame_pos + cyc;
    int new_frame = pos >= m->frame_cycles;
    if (new_frame) {
        m->frames += pos / m->frame_cycles;
        pos %= m->frame_cycles;
    }
    uint8_t vs = pos < m->vsync_cycles;
    if ((vs && !m->vsync) || (new_frame && vs == m->vsync && vs)) {
        if (m->vsync_irq_armed) {               /* CRTV, IR2 */
            m->vsync_irq_armed = 0;
            pc98_irq(m, 2, 1);
            pc98_irq(m, 2, 0);
        }
    }
    m->vsync = vs;
    m->frame_pos = pos;
    kbd_feed(m);
}

uint64_t pc98_run(Pc98 *m, uint64_t cycles) {
    uint64_t used = 0;
    while (used < cycles && !pc98_dead(m)) {
        uint64_t slice = cycles - used;
        if (slice > 256) slice = 256;
        uint64_t d = x86_run(&m->cpu, slice);
        pc98_advance(m, d);
        used += d;
    }
    return used;
}

int pc98_dead(const Pc98 *m) {
    return m->cpu.stop || (m->cpu.halted && !(m->cpu.flags & X86_IF) && !m->cpu.nmi_pending);
}

/* ------------------------------------------------------------------ set-up and boot */

void pc98_init(Pc98 *m, X86Model model, uint32_t cpu_hz) {
    memset(m, 0, sizeof *m);
    X86Bus bus = { .ctx = m, .read8 = bus_read, .write8 = bus_write, .in8 = bus_in, .out8 = bus_out,
                   .intr_ack = bus_intr };
    x86_init(&m->cpu, model, &bus);
    x86_reset(&m->cpu);
    m->cpu_hz = cpu_hz ? cpu_hz : 8000000;
    m->pit_hz = (m->cpu_hz % 8000000 == 0) ? 1996800 : 2457600;
    m->frame_cycles = (uint32_t)((uint64_t)m->cpu_hz * 100 / 5642);     /* 56.42 Hz, the 24.8 kHz 400-line mode */
    m->vsync_cycles = m->frame_cycles / 11;                              /* about 40 of 440 lines */
    m->vsync = 1;
    pc98_pic_reset(&m->pic[0]);
    pc98_pic_reset(&m->pic[1]);
    pc98_gdc_reset(&m->gdc[0]);
    pc98_gdc_reset(&m->gdc[1]);
    m->gdc[0].display_on = 1;                   /* as the BIOS leaves it: text on, graphics off */
    m->kbd.mode_next = 1;
    m->sys_a = 0xFF;                            /* DIP switches: not modelled */
    m->sys_b = 0xFF;
    m->sys_c = 0xF8;                            /* buzzer off (bit 3 set) */
    m->nmi_enabled = 1;
    /* Text screen cleared the way the BIOS leaves it: spaces, white, visible. */
    for (unsigned i = 0; i < 0x2000; i += 2) {
        m->tvram[i] = 0x20;
        m->tvram[0x2000 + i] = 0xE1;
    }
    /* Digital palette: colour n shows as GRB value n. */
    m->pal_digital[0] = 0x37;
    m->pal_digital[1] = 0x26;
    m->pal_digital[2] = 0x15;
    m->pal_digital[3] = 0x04;
    /* Analog palette (16-colour mode): our own default, eight dim and eight bright GRB colours. */
    for (int i = 0; i < 16; i++) {
        uint8_t level = (i & 8) ? 15 : 10;
        m->pal_analog[i][0] = (i & 4) ? level : 0;
        m->pal_analog[i][1] = (i & 2) ? level : 0;
        m->pal_analog[i][2] = (i & 1) ? level : 0;
    }
    m->pal_analog[8][0] = m->pal_analog[8][1] = m->pal_analog[8][2] = 7;
}

int pc98_boot(Pc98 *m, const Pc98Disk *disk, uint8_t da_ua) {
    Pc98Sector s;
    if (!disk || pc98_disk_sector(disk, 0, 0, 1, &s) != PC98_DISK_OK || !s.size) return -1;
    uint32_t n = s.size;
    if (n > 0x10000) n = 0x10000;
    memcpy(m->ram + (PC98_BOOT_SEG << 4), s.data, n);
    X86Cpu *c = &m->cpu;
    memset(c->r, 0, sizeof c->r);
    c->r[X86_AX] = da_ua;
    c->s[X86_CS] = c->s[X86_DS] = c->s[X86_ES] = c->s[X86_SS] = PC98_BOOT_SEG;
    c->r[X86_SP] = 0;
    c->ip = 0;
    c->flags &= (uint16_t)~(X86_IF | X86_TF | X86_DF);
    c->halted = 0;
    c->stop = 0;
    return 0;
}

/* ------------------------------------------------------------------ output */

void pc98_text_dump(const Pc98 *m, char *buf) {
    char *o = buf;
    for (int y = 0; y < PC98_TEXT_ROWS; y++) {
        for (int x = 0; x < PC98_TEXT_COLS; x++) {
            unsigned cell = 2u * (unsigned)(y * PC98_TEXT_COLS + x);
            uint8_t lo = m->tvram[cell], hi = m->tvram[cell + 1], attr = m->tvram[0x2000 + cell];
            char ch;
            if (!(attr & PC98_ATTR_VISIBLE)) ch = ' ';
            else if (hi) ch = '?';                  /* two-byte JIS code: kanji */
            else if (lo < 0x20) ch = ' ';
            else if (lo < 0x7F) ch = (char)lo;
            else ch = '?';
            *o++ = ch;
        }
        *o++ = '\n';
    }
    *o = 0;
}

static uint32_t grb_rgb(unsigned grb) {
    return ((grb & 2) ? 0xFF0000u : 0) | ((grb & 4) ? 0x00FF00u : 0) | ((grb & 1) ? 0x0000FFu : 0);
}

static uint32_t colour(const Pc98 *m, unsigned idx) {
    if (m->mode_ff2_analog) {
        const uint8_t *p = m->pal_analog[idx & 15];
        return (uint32_t)(p[1] * 17) << 16 | (uint32_t)(p[0] * 17) << 8 | (uint32_t)(p[2] * 17);
    }
    idx &= 7;
    static const uint8_t reg[4] = { 3, 2, 1, 0 };   /* colours 0..3 sit in AEh, ACh, AAh, A8h (high nibble), 4..7 low */
    uint8_t v = m->pal_digital[reg[idx & 3]];
    return grb_rgb(idx < 4 ? (v >> 4) & 7 : v & 7);
}

void pc98_render(const Pc98 *m, uint32_t *rgb, int stride, const uint8_t *font8x16) {
    int page = m->disp_page & 1;
    int graphics = m->gdc[1].display_on;
    for (int y = 0; y < 400; y++) {
        uint32_t *row = rgb + (size_t)y * (size_t)stride;
        for (int xb = 0; xb < 80; xb++) {
            unsigned o = (unsigned)(y * 80 + xb);
            uint8_t b = m->gvram[page][0][o], r = m->gvram[page][1][o], g = m->gvram[page][2][o], in = m->gvram[page][3][o];
            for (int bit = 0; bit < 8; bit++) {
                unsigned sh = 7u - (unsigned)bit;
                unsigned idx = ((b >> sh) & 1) | ((r >> sh) & 1) << 1 | ((g >> sh) & 1) << 2 | ((in >> sh) & 1) << 3;
                row[xb * 8 + bit] = graphics ? colour(m, idx) : 0;
            }
        }
    }
    if (!font8x16 || !m->gdc[0].display_on) return;
    for (int cy = 0; cy < PC98_TEXT_ROWS; cy++) {
        for (int cx = 0; cx < PC98_TEXT_COLS; cx++) {
            unsigned cell = 2u * (unsigned)(cy * PC98_TEXT_COLS + cx);
            uint8_t code = m->tvram[cell], hi = m->tvram[cell + 1], attr = m->tvram[0x2000 + cell];
            if (!(attr & PC98_ATTR_VISIBLE) || hi) continue;     /* hidden, or kanji (no kanji font) */
            uint32_t fg = grb_rgb((unsigned)(attr >> 5) & 7);
            for (int ly = 0; ly < 16; ly++) {
                uint8_t bits = font8x16[code * 16 + ly];
                if (attr & PC98_ATTR_REVERSE) bits = (uint8_t)~bits;
                if ((attr & PC98_ATTR_UNDERLINE) && ly == 15) bits = 0xFF;
                uint32_t *row = rgb + (size_t)(cy * 16 + ly) * (size_t)stride + cx * 8;
                for (int bit = 0; bit < 8; bit++) if (bits & (0x80 >> bit)) row[bit] = fg;
            }
        }
    }
}
