/* idle: finds the games' busy waits and moves the machine's clock past them (idle.h has the argument for why that is exact). */
#include "idle.h"
#include <string.h>

/* ---------------------------------------------------------------- the probe: a wrapped bus */

static void capture(IdleState *st, const X86Cpu *c) {
    memset(st, 0, sizeof *st);
    memcpy(st->r, c->r, sizeof st->r);
    memcpy(st->rh, c->rh, sizeof st->rh);
    memcpy(st->s, c->s, sizeof st->s);
    st->ip = c->ip;
    st->flags = c->flags;
    st->flags_hi = c->flags_hi;
    st->cr0 = c->cr0;
    st->halted = c->halted;
    st->in_rep = c->in_rep;
    st->shadow = c->shadow;
    st->nmi = c->nmi_pending;
}

static void w_trace(void *ctx, X86Cpu *c) {
    IdleProbe *p = ctx;
    if (p->nrec < IDLE_MAX_RECORDS) {
        IdleRecord *r = &p->rec[p->nrec++];
        capture(&r->st, c);
        r->cycles = c->cycles;
        r->instructions = c->instructions;
        r->nw = p->nw;
        r->np = p->np;
        r->ni = p->ni;
        r->na = p->na;
        r->nh = p->nh;
    } else p->overflow = 1;
    if (p->orig.trace) p->orig.trace(p->orig.ctx, c);
}

static uint8_t w_read8(void *ctx, uint32_t a) {
    IdleProbe *p = ctx;
    if (a >= PC98_RAM_SIZE && pc98gfx_mem_claims(a)) p->nh++;      /* display memory can have side effects when it is read (GRCG, EGC) */
    return p->orig.read8(p->orig.ctx, a);
}

static uint16_t w_read16(void *ctx, uint32_t a) {
    IdleProbe *p = ctx;
    if (a >= PC98_RAM_SIZE && pc98gfx_mem_claims(a)) p->nh++;
    return p->orig.read16(p->orig.ctx, a);
}

static void log_write(IdleProbe *p, uint32_t a) {
    if (a >= PC98_RAM_SIZE) { p->bad_write = 1; return; }          /* display memory, or nothing: not a loop we skip */
    if (p->nw < IDLE_MAX_WRITES) { p->wlog[p->nw].addr = a; p->wlog[p->nw].old = p->m->ram[a]; p->nw++; }
    else p->overflow = 1;
}

static void w_write8(void *ctx, uint32_t a, uint8_t v) {
    IdleProbe *p = ctx;
    log_write(p, a);
    p->orig.write8(p->orig.ctx, a, v);
}

static void w_write16(void *ctx, uint32_t a, uint16_t v) {
    IdleProbe *p = ctx;
    log_write(p, a);
    log_write(p, a + 1);
    p->orig.write16(p->orig.ctx, a, v);
}

static void log_port(IdleProbe *p, uint16_t port, int out) {
    if (p->np < IDLE_MAX_PORTS) { p->plog[p->np].port = port; p->plog[p->np].out = (uint8_t)out; p->np++; }
    else p->overflow = 1;
}

static uint8_t w_in8(void *ctx, uint16_t port) {
    IdleProbe *p = ctx;
    log_port(p, port, 0);
    return p->orig.in8(p->orig.ctx, port);
}

static void w_out8(void *ctx, uint16_t port, uint8_t v) {
    IdleProbe *p = ctx;
    log_port(p, port, 1);
    p->orig.out8(p->orig.ctx, port, v);
}

static int w_intr(void *ctx) {
    IdleProbe *p = ctx;
    int v = p->orig.intr_ack(p->orig.ctx);
    if (v >= 0) p->na++;
    return v;
}

static int w_soft(void *ctx, X86Cpu *cpu, uint8_t vector) {
    IdleProbe *p = ctx;
    p->ni++;
    return p->orig.soft_int ? p->orig.soft_int(p->orig.ctx, cpu, vector) : 0;
}

/* Wraps the machine's bus; returns 0 when it cannot (a bus with entry points this module does not know). */
static int probe_begin(IdleProbe *p, Pc98 *m) {
    X86Bus o = m->cpu.bus;
    if (!o.read8 || !o.write8 || !o.in8 || !o.out8 || !o.intr_ack || o.in16 || o.out16) return 0;
    p->orig = o;
    p->m = m;
    p->nrec = p->nw = p->np = p->ni = p->na = p->nh = 0;
    p->overflow = p->bad_write = 0;
    X86Bus b = o;
    b.ctx = p;
    b.read8 = w_read8;
    b.write8 = w_write8;
    b.read16 = o.read16 ? w_read16 : NULL;
    b.write16 = o.write16 ? w_write16 : NULL;
    b.in8 = w_in8;
    b.out8 = w_out8;
    b.intr_ack = w_intr;
    b.soft_int = w_soft;
    b.trace = w_trace;
    m->cpu.bus = b;
    p->active = 1;
    return 1;
}

static void probe_end(IdleProbe *p) {
    if (!p->active) return;
    p->m->cpu.bus = p->orig;
    p->active = 0;
}

/* ---------------------------------------------------------------- the horizon */

uint64_t idle_horizon(const Pc98 *m, int status) {
    /* An interrupt request that is pending now and that the CPU may take (IF set) would be taken at the very next instruction:
     * nothing may be skipped before it (the request can have been raised at the end of the stretch that was just run, after the CPU's
     * last chance to take it). With IF clear a pending request waits for the guest to enable interrupts, which a loop that only
     * polls never does: it does not stop a skip (a key struck while interrupts are off leaves IRQ 1 pending for good). */
    if ((m->cpu.flags & X86_IF) && (pc98_pic_pending(&m->pic[0]) >= 0 || pc98_pic_pending(&m->pic[1]) >= 0)) return 0;
    uint64_t pos = (m->frame_pos + (m->cpu.cycles - m->cpu_mark)) % m->frame_cycles;
    uint64_t h = m->frame_cycles - pos;                 /* never more than to the next frame boundary: the display's frame counter and the sound's FIFO are advanced a frame at a time */
    /* The sources of interrupts count only when the interrupt could be taken: a request from a source the PIC masks sits in its
     * IRR for the guest to find when it unmasks (a port write), and with IF clear nothing is taken at all. The PIT's counter 0 runs
     * in mode 3 with a count of 2 or 3 in the games' start-up (a free-running timer that nobody listens to): counting those as events
     * would stop every skip within 16 clocks. */
    if (m->cpu.flags & X86_IF) {
        if (m->vsync_irq_armed && !(m->pic[0].imr & 0x04)) {        /* the CRT's VSYNC interrupt (IRQ 2), armed by a write to port 64h */
            uint64_t left = m->frame_cycles - pos;
            if (left < h) h = left;
        }
        const Pc98PitCounter *c = &m->pit[0];                        /* the interval timer's counter 0 drives IRQ 0 */
        if (!(m->pic[0].imr & 0x01) && c->loaded && (c->mode == 2 || c->mode == 3 || c->armed)) {
            /* OUT rises when the ticks that arrive reach `count`; a tick is cpu_hz / pit_hz clocks, and the remainder carried in pit_frac
             * is less than one tick, so (count - 1) whole ticks is a lower bound for the wait. */
            uint64_t t = c->count > 1 ? (uint64_t)(c->count - 1) * m->cpu_hz / m->pit_hz : 0;
            if (t < h) h = t;
        }
        if (m->fm) {                                                 /* the FM board's timers, on the jumper's interrupt line */
            unsigned irq = m->fm->board_irq;
            int open = irq < 8 ? !(m->pic[0].imr & (1u << irq)) : (!(m->pic[1].imr & (1u << (irq - 8))) && !(m->pic[0].imr & 0x80));
            uint64_t t = open ? opna_ticks_to_irq(m->fm) : UINT64_MAX;
            if (t != UINT64_MAX) {
                t = t > 1 ? t - 1 : 0;
                if (t < h) h = t;
            }
        }
    }
    if (m->kbd.qlen && !m->kbd.rxrdy) h = 0;            /* a key is about to be delivered */
    if (status) {                                       /* the display status byte: VSYNC (bit 5) and HBLANK (bit 6) change at known clocks */
        uint64_t vs = pos < m->vsync_cycles ? m->vsync_cycles - pos : m->frame_cycles - pos;
        uint64_t off = pos % m->line_cycles, hb = m->line_cycles - m->hblank_cycles;
        uint64_t hn = off < hb ? hb - off : m->line_cycles - off;
        uint64_t t = vs < hn ? vs : hn;
        t = t > 1 ? t - 1 : 0;
        if (t < h) h = t;
    }
    return h;
}

/* ---------------------------------------------------------------- evaluating a probe */

static uint32_t linear_ip(const X86Cpu *c) { return ((uint32_t)c->s[X86_CS] << 4) + c->ip; }

/* Decides whether the probe that just ran ended in a pure loop; if so, how many clocks may be skipped (0 when the loop is
 * pure but the next event is too near). Returns -1 when the probe says nothing. */
static int64_t evaluate(IdleSkip *s, Pc98 *m, uint64_t room, uint64_t *period_out, uint64_t *instrs_out) {
    IdleProbe *p = &s->probe;
    X86Cpu *c = &m->cpu;
    if (p->overflow || p->bad_write || p->na || p->nrec < 3) return -1;
    if (c->stop || c->halted || c->in_rep || c->shadow || c->nmi_pending) return -1;
    IdleState end;
    capture(&end, c);
    int j = -1;                                         /* the latest earlier instruction boundary with the very same state */
    for (int i = (int)p->nrec - 1; i >= 0; i--)
        if (!memcmp(&p->rec[i].st, &end, sizeof end)) { j = i; break; }
    if (j < 0) return -1;
    const IdleRecord *r = &p->rec[j];
    uint64_t period = c->cycles - r->cycles, instrs = c->instructions - r->instructions;
    if (!period || !instrs) return -1;
    if (p->ni != r->ni || p->nh != r->nh) return -1;    /* an INT instruction ran, or display memory was read, inside the loop */
    int status = 0;
    for (unsigned k = r->np; k < p->np; k++) {          /* the ports the loop looks at */
        if (p->plog[k].out) return -1;
        switch (p->plog[k].port) {
        case 0x43: break;                               /* the keyboard's status */
        case 0x60: case 0xA0: status = 1; break;        /* the display controllers' status (VSYNC, HBLANK) */
        default: return -1;
        }
    }
    for (unsigned k = r->nw; k < p->nw; k++) {          /* every byte the loop writes must end up as it was: the same push, to the same place */
        unsigned first = 1;
        for (unsigned q = r->nw; q < k; q++) if (p->wlog[q].addr == p->wlog[k].addr) { first = 0; break; }
        if (first && m->ram[p->wlog[k].addr] != p->wlog[k].old) return -1;
    }
    uint32_t lo = 0xFFFFFFFFu, hi = 0;                  /* where the loop lives, for the next time */
    for (unsigned i = (unsigned)j; i < p->nrec; i++) {
        uint32_t lin = ((uint32_t)p->rec[i].st.s[X86_CS] << 4) + p->rec[i].st.ip;
        if (lin < lo) lo = lin;
        if (lin > hi) hi = lin;
    }
    s->loop_lo = lo > 16 ? lo - 16 : 0;
    s->loop_hi = hi + 16;
    uint64_t h = idle_horizon(m, status), margin = 128 + period;
    if (h <= margin) { s->near_event++; return 0; }
    h -= margin;
    if (h > room) { h = room; s->room_limited++; }
    uint64_t k = h / period * period;
    if (k < 2 * period || k < 64) { s->too_short++; return 0; }
    *period_out = period;
    *instrs_out = instrs;
    return (int64_t)k;
}

void idle_init(IdleSkip *s, int enabled) {
    memset(s, 0, sizeof *s);
    s->enabled = enabled;
    s->backoff = 1;
}

uint64_t idle_run(IdleSkip *s, Pc98 *m, uint64_t chunk, uint64_t room) {
    if (chunk > room) chunk = room;
    if (!s->enabled || pc98_dead(m) || m->cpu.halted) {
        uint64_t used = pc98_run(m, chunk);
        s->last_lin = linear_ip(&m->cpu);
        s->still = 0;
        return used;
    }
    int probe = 0;
    if (s->cooldown > 0) s->cooldown--;
    else {
        uint32_t lin = linear_ip(&m->cpu);
        probe = (s->loop_hi && lin >= s->loop_lo && lin <= s->loop_hi) || s->still >= 2;
    }
    uint64_t used;
    if (!probe || !probe_begin(&s->probe, m)) {
        used = pc98_run(m, chunk);
    } else {
        /* A loop seen before is probed for just long enough to show it again (a few of its turns); an unknown one for up to 512 clocks. */
        uint64_t len = s->last_period ? 4 * s->last_period + 64 : 512;
        if (len < 128) len = 128;
        if (len > 512) len = 512;
        if (len > room) len = room;
        used = pc98_run(m, len);
        probe_end(&s->probe);
        s->probes++;
        uint64_t period = 1, instrs = 1;
        int64_t k = pc98_dead(m) ? -1 : evaluate(s, m, used < room ? room - used : 0, &period, &instrs);
        if (k < 0) {                                    /* nothing to be gained here: stay out of the way for a while */
            s->cooldown = s->backoff;
            s->backoff = s->backoff < 64 ? s->backoff * 2 : 64;
            s->loop_lo = s->loop_hi = 0;
            s->last_period = 0;
        } else {
            s->hits++;
            s->backoff = 1;
            s->last_period = period;
            if (k > 0) {
                m->cpu.cycles += (uint64_t)k;
                m->cpu.instructions += (uint64_t)k / period * instrs;
                pc98_advance(m, (uint64_t)k);
                used += (uint64_t)k;
                s->skipped_clocks += (uint64_t)k;
                s->skipped_instrs += (uint64_t)k / period * instrs;
                s->skips++;
            } else s->cooldown = 1;
        }
    }
    uint32_t lin = linear_ip(&m->cpu);
    s->still = (lin + 192 > s->last_lin && lin < s->last_lin + 192 && !m->cpu.halted) ? s->still + 1 : 0;
    s->last_lin = lin;
    return used;
}
