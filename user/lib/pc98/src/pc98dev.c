/*
 * The chips of the PC-98 machine skeleton, each one a small state machine with no knowledge of the machine around it:
 *
 *  - 8259A interrupt controller: ICW1-ICW4 initialisation, IMR, IRR/ISR, fixed priority (IR0 highest), non-specific and
 *    specific EOI, automatic EOI, OCW3 register select and special mask mode, edge or level triggering.
 *    Not modelled: priority rotation (the rotate commands act as the matching EOI), poll mode, buffered mode.
 *  - 8253 interval timer counter: control word, LSB/MSB/both access, counter latch, modes 0 to 5 (1 and 5 need a gate
 *    edge that nobody drives here and behave like 0 and 4; 3 counts the way 2 does, so a read in mode 3 sees the count
 *    going down by one per clock instead of two), BCD reload values. OUT rising edges are counted for IRQ 0.
 *
 * (The uPD7220 display controllers belong to the display module, pc98gfx.c.)
 */
#include <pc98.h>
#include <string.h>

/* ------------------------------------------------------------------ 8259A */

void pc98_pic_reset(Pc98Pic *p) {
    memset(p, 0, sizeof *p);
    p->imr = 0xFF;
}

static int pic_highest(uint8_t bits) {
    for (int i = 0; i < 8; i++) if (bits & (1 << i)) return i;
    return -1;
}

void pc98_pic_write(Pc98Pic *p, int a0, uint8_t v) {
    if (!a0) {
        if (v & 0x10) {                         /* ICW1 */
            p->icw1 = v;
            p->init_step = 2;
            p->imr = 0;
            p->isr = 0;
            p->irr = 0;
            p->lines = 0;
            p->read_isr = 0;
            p->special_mask = 0;
            if (!(v & 1)) p->icw4 = 0;
            return;
        }
        if (v & 0x08) {                         /* OCW3 */
            if (v & 0x02) p->read_isr = v & 1;
            if (v & 0x40) p->special_mask = (v >> 5) & 1;
            return;
        }
        switch (v >> 5) {                       /* OCW2 */
        case 1: case 5: {                       /* (rotate on) non-specific EOI */
            int i = pic_highest(p->isr);
            if (i >= 0) p->isr &= (uint8_t)~(1 << i);
            break;
        }
        case 3: case 7:                         /* (rotate on) specific EOI */
            p->isr &= (uint8_t)~(1 << (v & 7));
            break;
        default: break;                         /* priority commands: not modelled */
        }
        return;
    }
    switch (p->init_step) {
    case 2:
        p->icw2 = v;
        p->init_step = (p->icw1 & 0x02) ? ((p->icw1 & 1) ? 4 : 0) : 3;
        break;
    case 3:
        p->icw3 = v;
        p->init_step = (p->icw1 & 1) ? 4 : 0;
        break;
    case 4:
        p->icw4 = v;
        p->init_step = 0;
        break;
    default:
        p->imr = v;                             /* OCW1 */
        break;
    }
}

uint8_t pc98_pic_read(Pc98Pic *p, int a0) {
    if (a0) return p->imr;
    return p->read_isr ? p->isr : p->irr;
}

void pc98_pic_set_line(Pc98Pic *p, int ir, int level) {
    uint8_t bit = (uint8_t)(1 << (ir & 7));
    if (p->icw1 & 0x08) {                       /* level triggered: IRR follows the line */
        if (level) p->irr |= bit; else p->irr &= (uint8_t)~bit;
    } else if (level && !(p->lines & bit)) {
        p->irr |= bit;                          /* rising edge */
    }
    if (level) p->lines |= bit; else p->lines &= (uint8_t)~bit;
}

int pc98_pic_pending(const Pc98Pic *p) {
    if (p->init_step) return -1;
    int i = pic_highest((uint8_t)(p->irr & ~p->imr));
    if (i < 0) return -1;
    if (!p->special_mask) {
        int s = pic_highest(p->isr);
        if (s >= 0 && s <= i) return -1;        /* something at least as important is in service */
    } else if (p->isr & (1 << i)) {
        return -1;
    }
    return i;
}

int pc98_pic_ack(Pc98Pic *p) {
    int i = pc98_pic_pending(p);
    if (i < 0) return -1;
    uint8_t bit = (uint8_t)(1 << i);
    if (!(p->icw1 & 0x08)) p->irr &= (uint8_t)~bit;
    if (!(p->icw4 & 0x02)) p->isr |= bit;      /* not in automatic EOI mode */
    return i;
}

/* ------------------------------------------------------------------ 8253 */

static uint32_t from_bcd(uint32_t v) {
    return ((v >> 12) & 15) * 1000 + ((v >> 8) & 15) * 100 + ((v >> 4) & 15) * 10 + (v & 15);
}

void pc98_pit_control(Pc98PitCounter pit[3], uint8_t v) {
    int sc = v >> 6;
    if (sc == 3) return;                        /* 8254 read-back: not on an 8253 */
    Pc98PitCounter *c = &pit[sc];
    int rw = (v >> 4) & 3;
    if (rw == 0) {                              /* counter latch */
        if (!c->latched) {
            c->latch = (uint16_t)c->count;
            c->latched = 1;
            c->read_lsb_next = 1;
        }
        return;
    }
    c->rw = (uint8_t)rw;
    c->mode = (v >> 1) & 7;
    if (c->mode > 5) c->mode -= 4;
    c->bcd = v & 1;
    c->write_lsb_next = 1;
    c->read_lsb_next = 1;
    c->latched = 0;
    c->loaded = 0;
    c->armed = 0;
    c->out = c->mode != 0;
}

void pc98_pit_write(Pc98PitCounter *c, uint8_t v) {
    uint32_t val;
    switch (c->rw) {
    case 1: val = v; break;
    case 2: val = (uint32_t)v << 8; break;
    case 3:
        if (c->write_lsb_next) { c->pending_lsb = v; c->write_lsb_next = 0; return; }
        val = c->pending_lsb | (uint32_t)v << 8;
        c->write_lsb_next = 1;
        break;
    default: return;                            /* never programmed */
    }
    if (c->bcd) val = from_bcd(val);
    c->reload = val ? val : (c->bcd ? 10000u : 65536u);
    c->count = c->reload;
    c->loaded = 1;
    c->armed = 1;
    if (c->mode == 0) c->out = 0;
}

uint8_t pc98_pit_read(Pc98PitCounter *c) {
    uint16_t v = c->latched ? c->latch : (uint16_t)c->count;
    switch (c->rw) {
    case 1: c->latched = 0; return (uint8_t)v;
    case 2: c->latched = 0; return (uint8_t)(v >> 8);
    case 3:
        if (c->read_lsb_next) { c->read_lsb_next = 0; return (uint8_t)v; }
        c->read_lsb_next = 1;
        c->latched = 0;
        return (uint8_t)(v >> 8);
    default: return 0xFF;
    }
}

void pc98_pit_advance(Pc98PitCounter *c, uint64_t ticks) {
    if (!c->loaded || !ticks) return;
    if (c->mode == 2 || c->mode == 3) {
        if (ticks < c->count) {
            c->count -= (uint32_t)ticks;
        } else {
            uint64_t n = ticks - c->count;
            c->edges += 1 + n / c->reload;
            c->count = c->reload - (uint32_t)(n % c->reload);
        }
        return;
    }
    /* Modes 0, 1, 4, 5: one event at the terminal count, then the counter wraps around and keeps going. */
    if (c->armed && ticks >= c->count) {
        c->armed = 0;
        if (!c->out || c->mode >= 4) c->edges++;   /* mode 0/1: OUT goes high; 4/5: the strobe pulse ends */
        c->out = 1;
    }
    uint32_t wrap = c->bcd ? 10000u : 65536u;
    int64_t left = (int64_t)c->count - (int64_t)(ticks % wrap);
    if (left <= 0) left += wrap;
    c->count = (uint32_t)left;
}
