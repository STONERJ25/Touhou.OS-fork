/*
 * The PC-98 machine: memory map, I/O port dispatcher, device timing, boot hand-over and the output. The display hardware
 * is pc98gfx.c's (this file routes the memory and ports to it), the FM board opna.c's. See include/pc98.h and docs/PC98.md
 * (the port table there lists what each port does here).
 */
#include <pc98.h>
#include <string.h>

/* ------------------------------------------------------------------ memory */

/* Watch mode: the first read of each BIOS work-area byte is reported, so a trace shows what the guest looks at. */
static void watch_read(Pc98 *m, uint32_t a) {
    unsigned i = a - 0x400u;
    if (m->work_seen[i >> 3] & (1u << (i & 7))) return;
    m->work_seen[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (m->event) m->event(m->host, PC98_EV_WORK_READ, a, m->ram[a]);
}

uint8_t pc98_read8(Pc98 *m, uint32_t a) {
    if (a < PC98_RAM_SIZE) {
        if (a == PC98_WA_KB_COUNT) m->bios.kb_polls++;      /* a program looking for keys (idle detection) */
        if (m->watch_work_area && a >= 0x400 && a < 0x600) watch_read(m, a);
        return m->ram[a];
    }
    if (pc98gfx_mem_claims(a)) return pc98gfx_mem_read8(&m->gfx, a);
    if (a >= PC98_BIOS_ROM_BASE && a < 0x100000u && m->bios.enabled) return m->rom[a - PC98_BIOS_ROM_BASE];
    return 0xFF;                                /* expansion area, the empty ROM, beyond 1 MiB */
}

/* The write watch of the tools (watch_hi = 0 is "off": one compare per write). */
static void watch_write(Pc98 *m, uint32_t a, uint8_t v) {
    if (!m->event) return;
    if (a >= m->watch_lo && a < m->watch_hi) { m->event(m->host, PC98_EV_MEM_WRITE, a, v); return; }
    if (m->watch_plane_hi && ((a >= PC98_GVRAM_B && a < PC98_GVRAM_B + 0x18000) || (a >= PC98_GVRAM_I && a < PC98_GVRAM_I + 0x8000))) {
        uint32_t off = (a - PC98_GVRAM_B) & 0x7FFF;
        if (off >= m->watch_plane_lo && off < m->watch_plane_hi) m->event(m->host, PC98_EV_MEM_WRITE, a, v);
    }
}

void pc98_write8(Pc98 *m, uint32_t a, uint8_t v) {
    if (m->watch_hi || m->watch_plane_hi) watch_write(m, a, v);
    if (a < PC98_RAM_SIZE) m->ram[a] = v;
    else if (pc98gfx_mem_claims(a)) pc98gfx_mem_write8(&m->gfx, a, v);
}

/* A word at an even graphics address is one operation to the EGC; everywhere else a word is two bytes, low byte first. */
uint16_t pc98_read16(Pc98 *m, uint32_t a) {
    if (a < 0x3FFu || (a >= 0x600u && a < PC98_RAM_SIZE - 1)) return (uint16_t)(m->ram[a] | m->ram[a + 1] << 8);
    if (!(a & 1) && pc98gfx_mem_claims(a) && a >= PC98GFX_PLANE_B) return pc98gfx_mem_read16(&m->gfx, a);
    uint8_t lo = pc98_read8(m, a);
    return (uint16_t)(lo | pc98_read8(m, a + 1) << 8);
}

void pc98_write16(Pc98 *m, uint32_t a, uint16_t v) {
    if ((m->watch_hi || m->watch_plane_hi) && a < PC98_RAM_SIZE - 1) { watch_write(m, a, (uint8_t)v); watch_write(m, a + 1, (uint8_t)(v >> 8)); }
    if (a < PC98_RAM_SIZE - 1) { m->ram[a] = (uint8_t)v; m->ram[a + 1] = (uint8_t)(v >> 8); return; }
    if (!(a & 1) && pc98gfx_mem_claims(a) && a >= PC98GFX_PLANE_B) {
        if (m->watch_hi || m->watch_plane_hi) { watch_write(m, a, (uint8_t)v); watch_write(m, a + 1, (uint8_t)(v >> 8)); }
        pc98gfx_mem_write16(&m->gfx, a, v);
        return;
    }
    pc98_write8(m, a, (uint8_t)v);
    pc98_write8(m, a + 1, (uint8_t)(v >> 8));
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

/* ---- machine time: the devices are advanced between the CPU's slices (pc98_advance), but a program that polls a status
 * port or a counter looks at the clock in the middle of one, so the readings that matter are brought up to the CPU's own
 * cycle count: T = what the devices were told + what the CPU has run since. */
static uint64_t live_time(const Pc98 *m) { return m->dev_time + (m->cpu.cycles - m->cpu_mark); }

static void fm_sync(Pc98 *m) {
    if (!m->fm) return;
    uint64_t t = live_time(m);
    if (t > m->fm_time) {
        opna_advance(m->fm, t - m->fm_time);
        m->fm_time = t;
    }
}

/* The beam: where in the frame the live clock is, for the status bits of the two GDCs. A line starts with its picture and
 * ends with the blanking (HBLANK); the frame starts with VSYNC. */
static int hblank_at(const Pc98 *m, uint64_t pos) { return (pos % m->line_cycles) >= m->line_cycles - m->hblank_cycles; }
static void beam_sync(Pc98 *m) {
    uint64_t pos = (m->frame_pos + (m->cpu.cycles - m->cpu_mark)) % m->frame_cycles;
    pc98gfx_set_retrace(&m->gfx, pos < m->vsync_cycles, hblank_at(m, pos));
}

static uint8_t unhandled_in(Pc98 *m, uint16_t port) {
    if (m->fm) {
        fm_sync(m);
        int v = opna_board_in(m->fm, port);
        if (v >= 0) return (uint8_t)v;
    }
    if (m->port_in) {
        int v = m->port_in(m->host, port);
        if (v >= 0) return (uint8_t)v;
    }
    m->unhandled++;
    m->last_unhandled_port = port;
    if (m->event) m->event(m->host, PC98_EV_UNHANDLED_IN, port, 0xFF);
    return 0xFF;
}

static uint8_t in8_dev(Pc98 *m, uint16_t port);
static void out8_dev(Pc98 *m, uint16_t port, uint8_t v);

uint8_t pc98_in8(Pc98 *m, uint16_t port) {
    uint8_t v = in8_dev(m, port);
    if (m->event) m->event(m->host, PC98_EV_IN, port, v);
    return v;
}

void pc98_out8(Pc98 *m, uint16_t port, uint8_t v) {
    if (m->event) m->event(m->host, PC98_EV_OUT, port, v);
    out8_dev(m, port, v);
}

static uint8_t in8_dev(Pc98 *m, uint16_t port) {
    uint32_t gv;
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
    case 0x60: case 0xA0: beam_sync(m); break;                  /* the status bytes: VSYNC and HBLANK of the live clock */
    case 0x71: case 0x3FD9: return pc98_pit_read(&m->pit[0]);
    case 0x73: case 0x3FDB: return pc98_pit_read(&m->pit[1]);
    case 0x75: case 0x3FDD: return pc98_pit_read(&m->pit[2]);
    case 0xF2: return m->a20 ? 0x00 : 0x01;     /* bit 0: A20 masked */
    default: break;
    }
    if (pc98gfx_port_read(&m->gfx, port, &gv, 1)) return (uint8_t)gv;
    return unhandled_in(m, port);
}

static void a20_set(Pc98 *m, int on) {
    m->a20 = (uint8_t)on;
    m->cpu.addr_mask = (on && m->cpu.model >= X86_MODEL_80286) ? 0xFFFFFF : 0xFFFFF;       /* (the 386 and 486 models too: their games and DOS extenders use the high memory area) */
}

static void out8_dev(Pc98 *m, uint16_t port, uint8_t v) {
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
    case 0x64: m->vsync_irq_armed = 1; return;  /* CRT interrupt reset: arms the next VSYNC interrupt */
    case 0x71: case 0x3FD9: pc98_pit_write(&m->pit[0], v); return;
    case 0x73: case 0x3FDB: pc98_pit_write(&m->pit[1], v); return;
    case 0x75: case 0x3FDD: pc98_pit_write(&m->pit[2], v); return;
    case 0x77: case 0x3FDF: pc98_pit_control(m->pit, v); return;
    case 0xF0: m->cpu_resets++; return;         /* CPU reset: needs the BIOS's resume path, not modelled */
    case 0xF2: a20_set(m, 1); return;
    case 0xF6:
        if (v == 0x02) a20_set(m, 1);
        else if (v == 0x03) a20_set(m, 0);
        return;
    default: break;
    }
    if (pc98gfx_port_write(&m->gfx, port, v, 1)) return;
    if (m->fm) {
        fm_sync(m);
        if (opna_board_out(m->fm, port, v) >= 0) return;
    }
    if (m->port_out && m->port_out(m->host, port, v) >= 0) return;
    m->unhandled++;
    m->last_unhandled_port = port;
    if (m->event) m->event(m->host, PC98_EV_UNHANDLED_OUT, port, v);
}

/* ------------------------------------------------------------------ CPU bus glue */

static uint8_t bus_read(void *ctx, uint32_t a) { return pc98_read8((Pc98 *)ctx, a); }
static void bus_write(void *ctx, uint32_t a, uint8_t v) { pc98_write8((Pc98 *)ctx, a, v); }
static uint16_t bus_read16(void *ctx, uint32_t a) { return pc98_read16((Pc98 *)ctx, a); }
static void bus_write16(void *ctx, uint32_t a, uint16_t v) { pc98_write16((Pc98 *)ctx, a, v); }
static uint8_t bus_in(void *ctx, uint16_t p) { return pc98_in8((Pc98 *)ctx, p); }
static void bus_out(void *ctx, uint16_t p, uint8_t v) { pc98_out8((Pc98 *)ctx, p, v); }

static int bus_intr(void *ctx) {
    Pc98 *m = (Pc98 *)ctx;
    int v = pc98_intr_ack(m);
    if (v >= 0 && m->event) m->event(m->host, PC98_EV_HW_INT, (uint32_t)v, 0);
    return v;
}

/* INT n: the BIOS's own stubs are answered in C; everything else goes through the vector table as usual. */
static int bus_soft_int(void *ctx, X86Cpu *cpu, uint8_t vector) {
    Pc98 *m = (Pc98 *)ctx;
    (void)cpu;
    if (m->bios.enabled && pc98_bios_soft_int(m, vector)) return 1;
    if (m->event) m->event(m->host, PC98_EV_SOFT_INT, vector, 0);
    return 0;
}

/* ------------------------------------------------------------------ time */

void pc98_advance(Pc98 *m, uint64_t cyc) {
    if (!cyc) return;
    m->dev_time += cyc;
    m->cpu_mark = m->cpu.cycles;
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
        uint64_t n = pos / m->frame_cycles;
        m->frames += n;
        pos %= m->frame_cycles;
        for (uint64_t i = 0; i < n && i < 4; i++) pc98gfx_vsync(&m->gfx);   /* the blink phases count frames */
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
    pc98gfx_set_retrace(&m->gfx, vs, hblank_at(m, pos));
    kbd_feed(m);
    fm_sync(m);
}

uint64_t pc98_run(Pc98 *m, uint64_t cycles) {
    uint64_t used = 0;
    while (used < cycles && !pc98_dead(m)) {
        uint64_t slice = cycles - used;
        if (slice > 256) slice = 256;
        uint64_t d = x86_run(&m->cpu, slice);
        pc98_advance(m, d);
        used += d;
        if (m->yield) { m->yield = 0; break; }
    }
    return used;
}

int pc98_dead(const Pc98 *m) {
    return m->cpu.stop || (m->cpu.halted && !(m->cpu.flags & X86_IF) && !m->cpu.nmi_pending);
}

/* ------------------------------------------------------------------ set-up and boot */

void pc98_init(Pc98 *m, X86Model model, uint32_t cpu_hz) {
    memset(m, 0, sizeof *m);
    X86Bus bus = { .ctx = m, .read8 = bus_read, .write8 = bus_write, .read16 = bus_read16, .write16 = bus_write16,
                   .in8 = bus_in, .out8 = bus_out, .intr_ack = bus_intr, .soft_int = bus_soft_int };
    x86_init(&m->cpu, model, &bus);
    x86_reset(&m->cpu);
    m->cpu_hz = cpu_hz ? cpu_hz : 8000000;
    m->pit_hz = (m->cpu_hz % 8000000 == 0) ? 1996800 : 2457600;
    m->frame_cycles = (uint32_t)((uint64_t)m->cpu_hz * 100 / 5642);     /* 56.42 Hz, the 24.8 kHz 400-line mode */
    m->vsync_cycles = m->frame_cycles / 11;                              /* about 40 of 440 lines */
    m->line_cycles = m->frame_cycles / 440;                              /* 440 lines of 40.3 us */
    if (!m->line_cycles) m->line_cycles = 1;
    m->hblank_cycles = m->line_cycles / 5;                               /* the line's blanking, about a fifth (approximate) */
    m->vsync = 1;
    pc98_pic_reset(&m->pic[0]);
    pc98_pic_reset(&m->pic[1]);
    pc98gfx_init(&m->gfx);                      /* text on, graphics off, spaces in white: the state the BIOS leaves */
    pc98gfx_set_retrace(&m->gfx, 1, 0);
    m->kbd.mode_next = 1;
    m->sys_a = 0xFF;                            /* DIP switches: not modelled */
    m->sys_b = 0xFF;
    m->sys_c = 0xF8;                            /* buzzer off (bit 3 set) */
    m->nmi_enabled = 1;
}

static void fm_irq(void *ctx, int level) {
    Pc98 *m = (Pc98 *)ctx;
    pc98_irq(m, m->fm->board_irq, level);
}

void pc98_attach_opna(Pc98 *m, Opna *fm, int board, int irq) {
    m->fm = fm;
    if (!fm) return;
    opna_board_init(fm, board, irq);
    opna_set_host_clock(fm, m->cpu_hz);
    opna_set_irq_callback(fm, fm_irq, m);
    m->fm_time = live_time(m);
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
    pc98gfx_text_dump(&m->gfx, buf);
}

void pc98_render(const Pc98 *m, uint32_t *rgb, int stride) {
    pc98gfx_render(&m->gfx, rgb, stride);
}
