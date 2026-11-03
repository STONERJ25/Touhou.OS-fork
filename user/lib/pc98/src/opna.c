/*
 * The YM2203 (OPN) and YM2608 (OPNA) sound chips of the PC-98's FM boards. include/opna.h has the interface,
 * docs/PC98_SOUND.md the register map, what is modelled and what is not.
 *
 * Sources: the YM2203 and YM2608 application manuals (register map, timers, prescaler, key code, detune and LFO tables,
 * SSG, rhythm and ADPCM procedures and formulas) and our own understanding of how a phase-modulation FM chip of this
 * family computes: a log-sine table and an exponent table instead of multipliers, a 10-bit attenuation envelope, a
 * global envelope counter that decides which samples a given rate may change the attenuation on. The tables are
 * computed here from their definitions (sin, log2, 2^x by our own series below); nothing is taken from another emulator.
 * Behaviour we could not pin down from the manuals is marked "unverified" here and in the docs.
 *
 * The clock structure, the key to the rest: the chip computes one operator ("slot") every 6 master clocks at the
 * default prescaler (1/6). The YM2203 has 12 slots (3 channels of 4 operators), so one FM sample is 72 phiM; the
 * YM2608 has 24, so 144 phiM. On the PC-98 (3.9936 MHz and 7.9872 MHz) both come out at 55466 Hz, which is why the
 * -86 board plays -26K music unchanged. Timer A counts FM samples, timer B every 16th. The SSG divides phiM by 4
 * (YM2203) or 8 (YM2608) and its tone counters step every 8 of those clocks.
 */
#include <opna.h>
#include <string.h>

/* ------------------------------------------------------------------ arithmetic for the tables (init only) */

#define M_PI_D 3.14159265358979323846
#define M_LN2_D 0.69314718055994530942

/* sin(x) by its Taylor series after reducing x to [-pi/2, pi/2]. Error well below 1e-15 for the arguments used here. */
static double m_sin(double x) {
    double two_pi = 2.0 * M_PI_D;
    while (x > M_PI_D) x -= two_pi;
    while (x < -M_PI_D) x += two_pi;
    if (x > M_PI_D / 2) x = M_PI_D - x;
    if (x < -M_PI_D / 2) x = -M_PI_D - x;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 20; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

/* e^x for |x| < 1 by its series. */
static double m_exp_small(double x) {
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 30; n++) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

/* 2^x: the integer part by doubling or halving, the fraction by e^(f ln 2). */
static double m_exp2(double x) {
    int n = 0;
    while (x >= 1.0) { x -= 1.0; n++; }
    while (x < 0.0) { x += 1.0; n--; }
    double v = m_exp_small(x * M_LN2_D);
    while (n > 0) { v *= 2.0; n--; }
    while (n < 0) { v *= 0.5; n++; }
    return v;
}

/* log2(x) for x > 0: x = m * 2^e with m in [1, 2), ln m = 2 atanh((m - 1) / (m + 1)) by its series. */
static double m_log2(double x) {
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0) { x *= 2.0; e--; }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0.0;
    for (int n = 0; n < 40; n++) {
        sum += term / (2 * n + 1);
        term *= z2;
    }
    return e + 2.0 * sum / M_LN2_D;
}

static double m_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) r = 0.5 * (r + x / r);
    return r;
}

static int round_pos(double v) { return (int)(v + 0.5); }

/* ------------------------------------------------------------------ chip constants */

/* The detune amounts of the manuals' DT1 table, in phase increment units (one unit = phiM / (sample_clocks * 2^20) Hz),
 * for DT = 1, 2, 3 and the 32 key codes; DT 5-7 subtract the same amounts, DT 0 and 4 do nothing. */
static const uint8_t detune_table[3][32] = {
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8 },
    { 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16 },
    { 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22 },
};

/* Envelope generator: which of 8 consecutive envelope-counter events change the attenuation, and by how much.
 * Rates 0-47 change it by 0 or 1 (the low two bits of the rate choose the pattern) on every 2^(11 - rate/4)-th counter
 * value; rates 48-59 change it on every event by 1-2, 2-4 or 4-8; rates 60-63 by 8. The patterns give each rate within
 * a group of four 4, 5, 6 or 7 changes out of 8, which is what makes the manuals' attack and decay times grow in steps
 * of a quarter octave. */
static const uint8_t eg_low[4][8] = {
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
};
static const uint8_t eg_high[4][8] = {
    { 1, 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 2, 1, 1, 1, 2 }, { 1, 2, 1, 2, 1, 2, 1, 2 }, { 1, 2, 2, 2, 1, 2, 2, 2 },
};

/* LFO (YM2608 22h): samples per LFO step (128 steps per period). The manual gives 3.98, 5.56, 6.02, 6.37, 6.88, 9.63,
 * 48.1 and 72.2 Hz at phiM = 8 MHz (55556 Hz samples); these whole dividers reproduce every one of them to 0.3 %. */
static const uint8_t lfo_divider[8] = { 109, 78, 72, 68, 63, 45, 9, 6 };
/* PM sensitivity (B4h bits 2-0): the deviation in cents at the top of the LFO wave (manual: 0 to 80 cents). */
static const uint16_t pm_cents_x10[8] = { 0, 34, 67, 100, 140, 200, 400, 800 };
/* AM sensitivity (B4h bits 5-4): 0, 1.4, 5.9, 11.8 dB = the triangle (0..126 attenuation steps) shifted right by: */
static const uint8_t am_shift[4] = { 8, 3, 1, 0 };

/* Algorithms: which operators modulate S2, S3, S4 (bit 0 = S1, 1 = S2, 2 = S3), and which are carriers. */
static const uint8_t alg_mod[8][4] = {
    { 0, 1, 2, 4 },     /* 0: S1 -> S2 -> S3 -> S4 */
    { 0, 0, 3, 4 },     /* 1: (S1 + S2) -> S3 -> S4 */
    { 0, 0, 2, 5 },     /* 2: (S1 + (S2 -> S3)) -> S4 */
    { 0, 1, 0, 6 },     /* 3: ((S1 -> S2) + S3) -> S4 */
    { 0, 1, 0, 4 },     /* 4: (S1 -> S2) + (S3 -> S4) */
    { 0, 1, 1, 1 },     /* 5: S1 -> S2, S3, S4 */
    { 0, 1, 0, 0 },     /* 6: (S1 -> S2) + S3 + S4 */
    { 0, 0, 0, 0 },     /* 7: S1 + S2 + S3 + S4 */
};
static const uint8_t alg_carriers[8] = { 8, 8, 8, 8, 10, 14, 14, 15 };

/* Register offsets +0, +4, +8, +Ch within an operator register group are S1, S3, S2, S4. */
static const uint8_t slot_of_offset[4] = { 0, 2, 1, 3 };

/* ------------------------------------------------------------------ tables */

static void build_tables(Opna *o) {
    /* Quarter wave of -log2(sin), sampled in the middle of each of the 256 steps, in 1/256 of a factor of two (so 256
     * units = 6.02 dB, the same scale as the envelope's attenuation times 4). */
    for (int i = 0; i < 256; i++)
        o->logsin[i] = (uint16_t)round_pos(-m_log2(m_sin((2 * i + 1) * M_PI_D / 1024.0)) * 256.0);
    /* 2^(-x/256) for the fractional part x of an attenuation, as an 11-bit mantissa 1024..2042: exptab[x] =
     * 1024 * 2^((255 - x) / 256). An attenuation a then gives (exptab[a & 255] << 2) >> (a >> 8), 8168 at a = 0. */
    for (int i = 0; i < 256; i++)
        o->exptab[i] = (uint16_t)round_pos(1024.0 * m_exp2((255 - i) / 256.0));
    /* SSG levels: 32 steps 1.5 dB apart (2^(-1/4)), level 0 silent, level 31 = 8191. */
    o->ssg_dac[0] = 0;
    for (int i = 1; i < 32; i++)
        o->ssg_dac[i] = (uint16_t)round_pos(8191.0 * m_exp2((i - 31) / 4.0));
    /* Rhythm levels in 0.75 dB steps (2^(-1/8) per step), Q15. */
    for (int i = 0; i < 96; i++)
        o->rhythm_gain[i] = (uint16_t)round_pos(32767.0 * m_exp2(-i / 8.0));
    /* Vibrato: the frequency ratio at step q (0..7) of a quarter of the LFO wave, minus one, in 1/65536. */
    for (int p = 0; p < 8; p++)
        for (int q = 0; q < 8; q++)
            o->pm_ratio[p][q] = (uint16_t)round_pos(65536.0 * (m_exp2(pm_cents_x10[p] * q / (7.0 * 12000.0)) - 1.0));
}

/* ------------------------------------------------------------------ interrupt line */

static void update_irq(Opna *o) {
    uint8_t live = o->flags & (uint8_t)~o->flag_mask;
    if (o->chip == OPNA_YM2608) live &= o->irq_enable;
    else live &= OPNA_ST_TIMER_A | OPNA_ST_TIMER_B;
    uint8_t level = live != 0;
    if (level == o->irq) return;
    o->irq = level;
    if (level) o->irq_count++;
    if (o->irq_cb) o->irq_cb(o->irq_ctx, level);
}

static void set_flag(Opna *o, uint8_t f) {
    if (o->chip == OPNA_YM2608 && (o->flag_mask & f)) return;   /* masked flags are not set (110h) */
    o->flags |= f;
    update_irq(o);
}

/* ------------------------------------------------------------------ clocks */

static void update_clocks(Opna *o) {
    /* 2 = 1/6 (power-on), 3 = 1/3 (2Eh after 2Dh), 0 or 1 = 1/2 (2Fh). */
    static const uint8_t fm_div[4] = { 2, 2, 6, 3 };
    static const uint8_t ssg_div[4] = { 1, 1, 4, 2 };
    unsigned sel = o->prescaler_sel & 3;
    unsigned slots = o->chip == OPNA_YM2608 ? 24 : 12;
    o->sample_clocks = fm_div[sel] * slots;
    o->ssg_clocks = 8u * ssg_div[sel] * (o->chip == OPNA_YM2608 ? 2u : 1u);
    if (o->sample_pos >= o->sample_clocks) o->sample_pos = o->sample_clocks - 1;
    /* the rhythm steps depend on the sample rate */
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        OpnaRhythm *d = &o->rhythm[i];
        /* drum samples per FM sample, 32.32: rate / (clock / sample_clocks) */
        d->step = o->clock ? (((uint64_t)d->rate * o->sample_clocks) << 32) / o->clock : 0;
    }
}

uint32_t opna_rate_num(const Opna *o) { return o->clock; }
uint32_t opna_rate_den(const Opna *o) { return o->sample_clocks; }

/* ------------------------------------------------------------------ SSG */

static void ssg_env_restart(OpnaSsg *s) {
    s->env_cnt = 0;
    s->env_pos = 0;
    s->env_hold = 0;
    s->env_attack = (s->reg[13] >> 2) & 1;
    s->env_level = s->env_attack ? 0 : 31;
}

static void ssg_write(Opna *o, unsigned r, uint8_t v) {
    OpnaSsg *s = &o->ssg;
    s->reg[r] = v;
    if (r == 13) ssg_env_restart(s);
}

static uint8_t ssg_read(const Opna *o, unsigned r) {
    const OpnaSsg *s = &o->ssg;
    if (r == 14) return (s->reg[7] & 0x40) ? s->reg[14] : s->port_in[0];
    if (r == 15) return (s->reg[7] & 0x80) ? s->reg[15] : s->port_in[1];
    return s->reg[r];
}

/* One SSG tick (phiSSG / 8): tone counters, the noise generator at half that rate, the envelope (one of its 32 steps
 * every EP ticks, so a whole ramp lasts 256 * EP SSG clocks as on the AY-3-8910). Returns the sum of the three
 * channels before the DC blocker (0 .. 3 * 8191). */
static int ssg_tick(Opna *o) {
    OpnaSsg *s = &o->ssg;
    for (int i = 0; i < 3; i++) {
        unsigned tp = s->reg[2 * i] | (unsigned)(s->reg[2 * i + 1] & 0x0F) << 8;
        if (!tp) tp = 1;
        if (++s->tone_cnt[i] >= tp) {
            s->tone_cnt[i] = 0;
            s->tone_out[i] ^= 1;
        }
    }
    s->noise_half ^= 1;
    if (s->noise_half) {
        unsigned np = s->reg[6] & 0x1F;
        if (!np) np = 1;
        if (++s->noise_cnt >= np) {
            s->noise_cnt = 0;
            /* 17 bits, x^17 + x^14 + 1: the new bit 16 is bit 0 xor bit 3 */
            uint32_t bit = (s->lfsr ^ (s->lfsr >> 3)) & 1;
            s->lfsr = (s->lfsr >> 1) | (bit << 16);
        }
    }
    unsigned ep = s->reg[11] | (unsigned)s->reg[12] << 8;
    if (!ep) ep = 1;
    if (!s->env_hold && ++s->env_cnt >= ep) {
        s->env_cnt = 0;
        if (++s->env_pos > 31) {
            uint8_t shape = s->reg[13];
            if (!(shape & 0x08)) {                  /* shapes 0-7: one ramp, then silence */
                s->env_hold = 1;
                s->env_level = 0;
            } else if (shape & 0x01) {              /* hold: at the end of the ramp, or flipped by ALT */
                s->env_hold = 1;
                s->env_level = (((shape >> 2) ^ (shape >> 1)) & 1) ? 31 : 0;
            } else {                                /* repeat, alternating the direction with ALT */
                if (shape & 0x02) s->env_attack ^= 1;
                s->env_pos = 0;
            }
        }
        if (!s->env_hold) s->env_level = s->env_attack ? s->env_pos : (uint8_t)(31 - s->env_pos);
    }
    unsigned noise = s->lfsr & 1;
    int sum = 0;
    for (int i = 0; i < 3; i++) {
        unsigned tone_off = (s->reg[7] >> i) & 1, noise_off = (s->reg[7] >> (i + 3)) & 1;
        if (!((s->tone_out[i] | tone_off) & (noise | noise_off))) continue;
        uint8_t vol = s->reg[8 + i];
        unsigned level = (vol & 0x10) ? s->env_level : ((vol & 0x0F) ? (vol & 0x0Fu) * 2 + 1 : 0);
        sum += o->ssg_dac[level];
    }
    return sum;
}

/* ------------------------------------------------------------------ FM: frequencies */

static unsigned keycode(unsigned fnum, unsigned block) {
    /* block and the two "note" bits: N4 = F11, N3 = F11 & (F10 | F9 | F8) | !F11 & F10 & F9 & F8 (manual) */
    unsigned f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    unsigned n3 = (f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8);
    return (block << 2) | (f11 << 1) | n3;
}

/* fnum and block of operator i of channel c (channel 3 has one per operator in the special and CSM modes) */
static void op_freq(const Opna *o, int c, int i, unsigned *fnum, unsigned *block) {
    if (c == 2 && (o->timer_ctrl & 0xC0) && i < 3) {
        *fnum = o->ch3_fnum[i];
        *block = o->ch3_block[i];
    } else {
        *fnum = o->ch[c].fnum;
        *block = o->ch[c].block;
    }
}

/* The phase increment per sample (20-bit phase, 1024 phase units of 1024 steps per cycle): F-number shifted by the
 * block, plus or minus the detune of this key code (17-bit wrap-around, as the adder has), times the multiple
 * (0 = one half). pm is the vibrato: a signed ratio in 1/65536 applied before the block shift is halved. */
static uint32_t phase_inc(unsigned fnum, unsigned block, unsigned dt, unsigned mul, int pm) {
    uint32_t base = (uint32_t)fnum << block;
    if (pm > 0) base += (base * (uint32_t)pm) >> 16;
    else if (pm < 0) base -= (base * (uint32_t)-pm) >> 16;
    uint32_t f = base >> 1;
    unsigned d = (dt & 3) ? detune_table[(dt & 3) - 1][keycode(fnum, block)] : 0;
    if (dt & 4) f -= d; else f += d;
    f &= 0x1FFFF;
    return mul ? (f * mul) & 0xFFFFF : f >> 1;
}

uint32_t opna_phase_inc(const Opna *o, unsigned fnum, unsigned block, unsigned dt, unsigned mul, int pm) {
    (void)o;
    return phase_inc(fnum & 2047, block & 7, dt & 7, mul & 15, pm);
}

/* ------------------------------------------------------------------ FM: envelope generator */

static unsigned eg_rate(const OpnaOp *op, unsigned kc) {
    unsigned r;
    switch (op->eg_state) {
    case OPNA_EG_ATTACK: r = op->ar; break;
    case OPNA_EG_DECAY: r = op->dr; break;
    case OPNA_EG_SUSTAIN: r = op->sr; break;
    default: r = op->rr * 2u + 1; break;
    }
    if (!r) return 0;
    r = r * 2 + (kc >> (3 - op->ks));
    return r > 63 ? 63 : r;
}

static unsigned eg_increment(unsigned rate, unsigned cnt) {
    if (rate < 2) return 0;
    if (rate < 48) {
        unsigned shift = 11 - (rate >> 2);
        if (cnt & ((1u << shift) - 1)) return 0;
        return eg_low[rate & 3][(cnt >> shift) & 7];
    }
    if (rate >= 60) return 8;
    return (unsigned)eg_high[rate & 3][cnt & 7] << ((rate >> 2) - 12);
}

static unsigned sustain_level(const OpnaOp *op) {
    return op->sl == 15 ? 31u * 32 : op->sl * 32u;      /* 3 dB steps; 15 means 93 dB */
}

static void eg_step(Opna *o, OpnaOp *op, unsigned kc) {
    unsigned rate = eg_rate(op, kc);
    unsigned inc = eg_increment(rate, o->eg_cnt);
    int att = op->att;
    int ssg = (op->ssgeg & 8) && op->eg_state != OPNA_EG_RELEASE;
    if (op->eg_state == OPNA_EG_ATTACK) {
        if (rate >= 62) att = 0;
        else if (inc) att -= ((att + 1) * (int)inc + 15) >> 4;     /* att += (~att * inc) >> 4, rounded down */
        if (att <= 0) {
            att = 0;
            op->eg_state = OPNA_EG_DECAY;
        }
    } else {
        att += (int)(ssg ? inc * 4 : inc);
        if (op->eg_state == OPNA_EG_DECAY && att >= (int)sustain_level(op)) op->eg_state = OPNA_EG_SUSTAIN;
    }
    if (ssg && att >= 0x200) {
        /* SSG-type envelope (approximate): the end of a ramp repeats it (attack again), alternates the output's
         * direction, or holds, the way the SSG's envelope shapes 8-15 do. An alternation turns round where it is (the
         * inverted output continues from the same level), so it needs no attack; a repeat with attack rate 62-63
         * starts again at once, as a key-on does. */
        uint8_t shape = op->ssgeg;
        if (shape & 1) {
            if ((shape & 2) && !(op->ssg_inv & 2)) op->ssg_inv ^= 1;
            op->ssg_inv |= 2;                       /* bit 1: holding */
            att = ((op->ssg_inv ^ (shape >> 2)) & 1) ? 0x200 : 0x3FF;
        } else if (shape & 2) {
            op->ssg_inv ^= 1;
            att = 0;
            op->eg_state = OPNA_EG_DECAY;
        } else {
            op->eg_state = OPNA_EG_ATTACK;
            if (eg_rate(op, kc) >= 62) {
                att = 0;
                op->eg_state = OPNA_EG_DECAY;
            } else {
                att = 0x200;
            }
        }
    }
    if (att > 1023) att = 1023;
    op->att = (uint16_t)att;
}

/* The attenuation the operator outputs (before TL and AM): the SSG-type envelope may invert it. */
static unsigned eg_out(const OpnaOp *op) {
    if ((op->ssgeg & 8) && op->eg_state != OPNA_EG_RELEASE && ((op->ssg_inv ^ (op->ssgeg >> 2)) & 1))
        return (0x200u - op->att) & 0x3FF;
    return op->att;
}

static void op_key(Opna *o, int c, int i) {
    OpnaOp *op = &o->ch[c].op[i];
    uint8_t on = op->key_reg | op->key_csm;
    if (on && !op->keyed) {
        unsigned fnum, block;
        op_freq(o, c, i, &fnum, &block);
        op->phase = 0;
        op->ssg_inv = 0;
        op->eg_state = OPNA_EG_ATTACK;
        if (eg_rate(op, keycode(fnum, block)) >= 62) {
            op->att = 0;
            op->eg_state = OPNA_EG_DECAY;
        }
    } else if (!on && op->keyed) {
        if ((op->ssgeg & 8) && ((op->ssg_inv ^ (op->ssgeg >> 2)) & 1)) op->att = (uint16_t)((0x200u - op->att) & 0x3FF);
        op->ssg_inv = 0;
        op->eg_state = OPNA_EG_RELEASE;
    }
    op->keyed = on;
}

/* ------------------------------------------------------------------ FM: operators and channels */

/* One operator lookup: the 10-bit phase (top bits of the accumulator plus the modulation) picks a log-sine value, the
 * attenuation (x4 to the same 1/256-octave scale) is added, and the exponent table turns the sum back into a linear
 * 14-bit signed value. Half 2 of the wave is the negative of half 1, and each half is two mirrored quarters. */
static int op_output(const Opna *o, uint32_t phase, int mod, unsigned att) {
    unsigned p = (unsigned)((int)(phase >> 10) + mod) & 0x3FF;
    unsigned idx = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF);
    unsigned a = o->logsin[idx] + (att << 2);
    if (a >= 0x1A00) return 0;
    int v = (int)(((unsigned)o->exptab[a & 0xFF] << 2) >> (a >> 8));
    return (p & 0x200) ? -v : v;
}

int opna_op_output(const Opna *o, uint32_t phase, int mod, unsigned att) {
    return op_output(o, phase & 0xFFFFF, mod, att > 1023 ? 1023 : att);
}

static int lfo_on(const Opna *o) { return o->chip == OPNA_YM2608 && (o->lfo & 8); }

/* The vibrato ratio (signed, 1/65536) for a channel's PM sensitivity at the LFO's current step: a triangle in 32
 * steps per period, 8 per quarter. */
static int pm_now(const Opna *o, unsigned pms) {
    if (!pms || !lfo_on(o)) return 0;
    unsigned step = o->lfo_cnt >> 2;            /* 0..31 */
    unsigned q = (step & 8) ? 7 - (step & 7) : (step & 7);
    int r = o->pm_ratio[pms][q];
    return (step & 16) ? -r : r;
}

static unsigned am_now(const Opna *o) {
    if (!lfo_on(o)) return 0;
    unsigned c = o->lfo_cnt;
    return (c < 64 ? c : 127 - c) * 2;          /* 0..126 attenuation steps = 0..11.8 dB */
}

/* Computes one sample of channel c (operators in the chip's order S1, S3, S2, S4: an operator modulated by one that
 * comes later in that order, S2 into S3, sees the previous sample's output). Returns the sum of the carriers. */
static int fm_channel(Opna *o, int c, int eg_tick, unsigned am) {
    static const uint8_t order[4] = { 0, 2, 1, 3 };
    OpnaChannel *ch = &o->ch[c];
    int pm = pm_now(o, ch->pms);
    int sum = 0;
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        OpnaOp *op = &ch->op[i];
        unsigned fnum, block;
        op_freq(o, c, i, &fnum, &block);
        if (eg_tick) eg_step(o, op, keycode(fnum, block));
        int mod;
        if (i == 0) {
            /* self feedback: the mean of the last two outputs, scaled so FB 7 is a modulation of 4 pi */
            mod = ch->fb ? (ch->fb_mem[0] + ch->fb_mem[1]) >> (10 - ch->fb) : 0;  /* (arithmetic shift) */
        } else {
            unsigned m = alg_mod[ch->alg][i];
            int in = 0;
            if (m & 1) in += ch->op[0].out;
            if (m & 2) in += ch->op[1].out;
            if (m & 4) in += ch->op[2].out;
            mod = in >> 1;
        }
        unsigned att = eg_out(op) + ((unsigned)op->tl << 3);
        if (op->am && o->chip == OPNA_YM2608) att += am >> am_shift[ch->ams];
        if (att > 1023) att = 1023;
        int v = op_output(o, op->phase, mod, att);
        op->out = (int16_t)v;
        if (i == 0) {
            ch->fb_mem[1] = ch->fb_mem[0];
            ch->fb_mem[0] = (int16_t)v;
        }
        op->phase = (op->phase + phase_inc(fnum, block, op->dt, op->mul, pm)) & 0xFFFFF;
        if (alg_carriers[ch->alg] & (1 << i)) sum += v;
    }
    if (sum > 8191) sum = 8191;                 /* the channel accumulator is 14 bits (unverified) */
    if (sum < -8192) sum = -8192;
    return sum;
}

/* ------------------------------------------------------------------ FM: registers */

static void fm_write(Opna *o, int base, unsigned r, uint8_t v) {
    unsigned cn = r & 3;
    if (cn == 3) return;
    int c = base + (int)cn;
    OpnaChannel *ch = &o->ch[c];
    if (r >= 0xA0) {
        switch (r & 0xFC) {
        case 0xA0:
            ch->fnum = (uint16_t)((o->fnum_latch & 7) << 8 | v);
            ch->block = (o->fnum_latch >> 3) & 7;
            break;
        case 0xA4: o->fnum_latch = v & 0x3F; break;
        case 0xA8:
            if (base == 0) {                    /* A8h S3, A9h S1, AAh S2 */
                static const uint8_t op_of[3] = { 2, 0, 1 };
                o->ch3_fnum[op_of[cn]] = (uint16_t)((o->ch3_latch & 7) << 8 | v);
                o->ch3_block[op_of[cn]] = (o->ch3_latch >> 3) & 7;
            }
            break;
        case 0xAC: if (base == 0) o->ch3_latch = v & 0x3F; break;
        case 0xB0:
            ch->alg = v & 7;
            ch->fb = (v >> 3) & 7;
            break;
        case 0xB4:
            if (o->chip == OPNA_YM2608) {
                ch->pan = v & 0xC0;
                ch->ams = (v >> 4) & 3;
                ch->pms = v & 7;
            }
            break;
        default: break;
        }
        return;
    }
    OpnaOp *op = &ch->op[slot_of_offset[(r >> 2) & 3]];
    switch (r >> 4) {
    case 3: op->dt = (v >> 4) & 7; op->mul = v & 15; break;
    case 4: op->tl = v & 0x7F; break;
    case 5: op->ks = v >> 6; op->ar = v & 0x1F; break;
    case 6: op->am = o->chip == OPNA_YM2608 ? v >> 7 : 0; op->dr = v & 0x1F; break;
    case 7: op->sr = v & 0x1F; break;
    case 8: op->sl = v >> 4; op->rr = v & 15; break;
    case 9: op->ssgeg = v & 15; break;
    default: break;
    }
}

static int channels(const Opna *o) {
    return (o->chip == OPNA_YM2608 && o->six_channels) ? 6 : 3;
}

static void key_write(Opna *o, uint8_t v) {
    unsigned sel = v & 7;
    if ((sel & 3) == 3) return;
    int c = (int)(sel & 3) + ((sel & 4) ? 3 : 0);
    if (c >= channels(o)) return;
    for (int i = 0; i < 4; i++) {
        o->ch[c].op[i].key_reg = (v >> (4 + i)) & 1;      /* bit 4 S1, 5 S2, 6 S3, 7 S4 */
        op_key(o, c, i);
    }
}

static void timer_write(Opna *o, uint8_t v) {
    uint8_t old = o->timer_ctrl;
    o->timer_ctrl = v & 0xCF;
    if (v & 0x10) o->flags &= (uint8_t)~OPNA_ST_TIMER_A;
    if (v & 0x20) o->flags &= (uint8_t)~OPNA_ST_TIMER_B;
    if ((v & 1) && !(old & 1)) o->ta_left = (uint16_t)(1024 - o->ta_value);
    if ((v & 2) && !(old & 2)) {
        o->tb_left = (uint16_t)(256 - o->tb_value);
        o->tb_pre = 0;
    }
    update_irq(o);
}

static void control_write(Opna *o, unsigned r, uint8_t v) {
    switch (r) {
    case 0x22:
        if (o->chip == OPNA_YM2608) {
            o->lfo = v & 0x0F;
            if (!(v & 8)) { o->lfo_cnt = 0; o->lfo_div = 0; }
        }
        break;
    case 0x24: o->ta_value = (uint16_t)((o->ta_value & 3) | (unsigned)v << 2); break;
    case 0x25: o->ta_value = (uint16_t)((o->ta_value & 0x3FC) | (v & 3)); break;
    case 0x26: o->tb_value = v; break;
    case 0x27: timer_write(o, v); break;
    case 0x28: key_write(o, v); break;
    case 0x29:
        if (o->chip == OPNA_YM2608) {
            o->six_channels = v >> 7;
            o->irq_enable = v & 0x1F;
            update_irq(o);
        }
        break;
    default: break;                             /* 21h test register, 2Dh-2Fh (see opna_write_addr), the unused ones */
    }
}

/* ------------------------------------------------------------------ rhythm (YM2608 10h-1Dh) */

static void rhythm_write(Opna *o, unsigned r, uint8_t v) {
    if (r == 0x10) {
        for (int i = 0; i < OPNA_RHYTHMS; i++) {
            if (!(v & (1 << i))) continue;
            OpnaRhythm *d = &o->rhythm[i];
            if (v & 0x80) d->playing = 0;           /* DM: dump (stop) */
            else { d->playing = d->pcm != NULL; d->pos = 0; }
        }
    } else if (r == 0x11) {
        o->rhythm_tl = v & 0x3F;
    } else if (r >= 0x18 && r <= 0x1D) {
        o->rhythm[r - 0x18].level = v & 0xDF;
    }
}

void opna_set_rhythm_sample(Opna *o, int index, const int16_t *pcm, uint32_t count, uint32_t rate) {
    if (index < 0 || index >= OPNA_RHYTHMS) return;
    OpnaRhythm *d = &o->rhythm[index];
    d->pcm = count ? pcm : NULL;
    d->count = count;
    d->rate = rate;
    d->playing = 0;
    d->pos = 0;
    update_clocks(o);
}

static void rhythm_sample(Opna *o, int32_t *l, int32_t *r) {
    int32_t sl = 0, sr = 0;
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        OpnaRhythm *d = &o->rhythm[i];
        if (!d->playing) continue;
        uint32_t at = (uint32_t)(d->pos >> 32);
        if (at >= d->count) { d->playing = 0; continue; }
        int32_t s0 = d->pcm[at], s1 = at + 1 < d->count ? d->pcm[at + 1] : 0;
        uint32_t frac = (uint32_t)(d->pos >> 16) & 0xFFFF;
        int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 16);
        d->pos += d->step;
        unsigned att = (63u - o->rhythm_tl) + (31u - (d->level & 0x1F));
        int32_t v = (int32_t)(((int64_t)s * o->rhythm_gain[att]) >> 15);
        if (d->level & 0x80) sl += v;
        if (d->level & 0x40) sr += v;
    }
    *l = sl;
    *r = sr;
}

/* ------------------------------------------------------------------ ADPCM-B (YM2608 bank 1 00h-10h) */

enum { OPNA_ADM_IDLE = 0, OPNA_ADM_WRITE, OPNA_ADM_READ, OPNA_ADM_PLAY };

/* How long one byte of a CPU memory access keeps BRDY low, in phiM (unverified; short against any CPU loop). */
#define ADPCM_BYTE_CLOCKS 32

static unsigned adpcm_shift(const Opna *o) {
    /* The address registers count 32-byte units with 8-bit wide RAM (101h bit 1 = 1) and 4-byte units with 1-bit
     * wide RAM; the -86 board's drivers select 8-bit RAM (101h = 02h). */
    return (o->adpcm.reg[1] & 0x02) ? 5 : 2;
}

static uint32_t adpcm_reg16(const Opna *o, unsigned lo) {
    return o->adpcm.reg[lo] | (uint32_t)o->adpcm.reg[lo + 1] << 8;
}

/* The chip's address counter covers its whole address space (2 MiB with 8-bit RAM); the board's 256 KiB answer to the
 * low 18 bits of it, so the RAM repeats. Stop and limit compare with the full counter. */
#define ADPCM_RAM_BYTE(o, addr) ((o)->adpcm_ram[(addr) & (OPNA_ADPCM_RAM - 1)])

static void adpcm_load_addresses(Opna *o) {
    OpnaAdpcm *a = &o->adpcm;
    unsigned sh = adpcm_shift(o);
    a->addr = adpcm_reg16(o, 2) << sh;
    a->end = ((adpcm_reg16(o, 4) + 1) << sh) - 1;
    a->limit = (adpcm_reg16(o, 0x0C) + 1) << sh;
}

/* Moves the access pointer on by one byte: the stop address ends the access (EOS), the limit wraps it to 0. */
static int adpcm_next(Opna *o) {
    OpnaAdpcm *a = &o->adpcm;
    int at_end = a->addr == a->end;
    a->addr++;
    if (a->addr >= a->limit || a->addr >= (0x10000u << adpcm_shift(o))) a->addr = 0;
    return at_end;
}

static void adpcm_decoder_reset(OpnaAdpcm *a) {
    a->x = 0;
    a->prev = 0;
    a->step = 127;
    a->pos = 0;
    a->nibble_hi = 1;
}

static void adpcm_control1(Opna *o, uint8_t v) {
    OpnaAdpcm *a = &o->adpcm;
    if (v & 0x01) {                             /* RESET: stop everything */
        a->mode = OPNA_ADM_IDLE;
        a->playing = 0;
        return;
    }
    if (v & 0x80) {                             /* START */
        a->playing = 1;
        a->mode = OPNA_ADM_PLAY;
        adpcm_load_addresses(o);
        adpcm_decoder_reset(a);
        return;
    }
    a->playing = 0;
    if ((v & 0x60) == 0x60) a->mode = OPNA_ADM_WRITE;   /* REC + MEMDATA: the CPU writes the RAM */
    else if (v & 0x20) a->mode = OPNA_ADM_READ;         /* MEMDATA: the CPU reads it */
    else a->mode = OPNA_ADM_IDLE;
    /* The addresses are usually written after the mode, so the pointer is loaded by the first data access (memory
     * write) or by the two dummy reads the manual asks for (memory read). Entering a memory mode makes the chip
     * ready at once (one BRDY edge, for an interrupt-driven transfer). */
    a->first = 1;
    a->dummy = 2;
    a->ready_at = o->now;
    a->brdy_pending = a->mode != OPNA_ADM_IDLE;
}

/* A CPU memory access of one byte starts: BRDY drops and comes back ADPCM_BYTE_CLOCKS later. */
static void adpcm_byte_started(Opna *o) {
    o->flags &= (uint8_t)~OPNA_ST_BRDY;
    o->adpcm.ready_at = o->now + ADPCM_BYTE_CLOCKS;
    o->adpcm.brdy_pending = 1;
    update_irq(o);
}

static void adpcm_write(Opna *o, unsigned r, uint8_t v) {
    OpnaAdpcm *a = &o->adpcm;
    a->reg[r] = v;
    switch (r) {
    case 0x00: adpcm_control1(o, v); break;
    case 0x08:
        if (a->mode == OPNA_ADM_WRITE) {
            if (a->first) { adpcm_load_addresses(o); a->first = 0; }
            if (o->now < a->ready_at) a->early_writes++;   /* a real chip may drop it (unverified); we keep it */
            ADPCM_RAM_BYTE(o, a->addr) = v;
            a->mem_writes++;
            adpcm_byte_started(o);
            if (adpcm_next(o)) set_flag(o, OPNA_ST_EOS);
        }
        break;
    case 0x10:
        if (v & 0x80) o->flags = 0;             /* IRQ RESET: every flag; the mask stays */
        else o->flag_mask = v & 0x1F;
        update_irq(o);
        break;
    default: break;
    }
}

static uint8_t adpcm_read_data(Opna *o) {
    OpnaAdpcm *a = &o->adpcm;
    if (a->mode != OPNA_ADM_READ) return 0;
    if (a->dummy) {                             /* the first two reads load the pointer and fill the pipeline */
        a->dummy--;
        adpcm_load_addresses(o);
        adpcm_byte_started(o);
        return a->read_latch;
    }
    a->read_latch = ADPCM_RAM_BYTE(o, a->addr);
    a->mem_reads++;
    adpcm_byte_started(o);
    if (adpcm_next(o)) set_flag(o, OPNA_ST_EOS);
    return a->read_latch;
}

/* BRDY. The manual's procedures reset the flags (110h = 80h) after every byte and wait for BRDY before the next one,
 * and in the read procedure that reset comes after the read that started the next byte: so the status bit has to
 * read 1 whenever the chip is ready, whatever the reset did. We keep a latched flag (one edge per byte, for the
 * interrupt) and report the ready level in status 1 (unverified, but it is what makes both procedures work). */
static void adpcm_brdy(Opna *o) {
    OpnaAdpcm *a = &o->adpcm;
    if (a->brdy_pending && o->now >= a->ready_at) {
        a->brdy_pending = 0;
        if (a->mode == OPNA_ADM_WRITE || a->mode == OPNA_ADM_READ) set_flag(o, OPNA_ST_BRDY);
    }
}

static int adpcm_ready_level(const Opna *o) {
    const OpnaAdpcm *a = &o->adpcm;
    if (a->mode != OPNA_ADM_WRITE && a->mode != OPNA_ADM_READ) return 0;
    return o->now >= a->ready_at;
}

static int32_t adpcm_decode(OpnaAdpcm *a, unsigned nib) {
    /* manual: X(n+1) = X(n) + (1 - 2 L4) (L3 + L2/2 + L1/4 + 1/8) D(n); D(n+1) = f(L3 L2 L1) D(n), f = 0.9 (x4), 1.2,
     * 1.6, 2.0, 2.4; D in 127..24576. The multipliers in 1/64 (the chip's exact rounding is unverified). */
    static const uint8_t mult[8] = { 57, 57, 57, 57, 77, 102, 128, 153 };
    unsigned m = nib & 7;
    int32_t d = (int32_t)(((2 * m + 1) * (uint32_t)a->step) >> 3);
    a->x += (nib & 8) ? -d : d;
    if (a->x > 32767) a->x = 32767;
    if (a->x < -32768) a->x = -32768;
    a->step = (int32_t)(((uint32_t)a->step * mult[m]) >> 6);
    if (a->step < 127) a->step = 127;
    if (a->step > 24576) a->step = 24576;
    return a->x;
}

/* One FM sample of ADPCM playback: DELTA-N / 65536 decoded samples per FM sample, linear interpolation between the last
 * two (the chip interpolates too; how exactly is unverified). */
static void adpcm_sample(Opna *o, int32_t *l, int32_t *r) {
    OpnaAdpcm *a = &o->adpcm;
    *l = *r = 0;
    if (!a->playing) return;
    uint32_t dn = adpcm_reg16(o, 9);
    a->pos += dn;
    while (a->pos >= 0x10000 && a->playing) {
        a->pos -= 0x10000;
        uint8_t byte = ADPCM_RAM_BYTE(o, a->addr);
        unsigned nib = a->nibble_hi ? byte >> 4 : byte & 15;
        a->prev = a->x;
        adpcm_decode(a, nib);
        a->nibble_hi ^= 1;
        if (a->nibble_hi && adpcm_next(o)) {    /* both nibbles of the stop byte played */
            set_flag(o, OPNA_ST_EOS);
            if (a->reg[0] & 0x10) {             /* REPEAT */
                adpcm_load_addresses(o);
                adpcm_decoder_reset(a);
            } else {
                a->playing = 0;
                a->mode = OPNA_ADM_IDLE;
            }
        }
    }
    if (!a->playing) return;
    int32_t s = a->prev + (int32_t)(((int64_t)(a->x - a->prev) * (int64_t)a->pos) >> 16);
    s = (int32_t)(((int64_t)s * a->reg[0x0B]) >> 8);
    if (a->reg[0] & 0x08) return;               /* SPOFF */
    if (a->reg[1] & 0x80) *l = s;
    if (a->reg[1] & 0x40) *r = s;
}

/* ------------------------------------------------------------------ timers */

static void timers_sample(Opna *o) {
    if (o->timer_ctrl & 1) {
        if (--o->ta_left == 0) {
            o->ta_left = (uint16_t)(1024 - o->ta_value);
            if (o->timer_ctrl & 4) set_flag(o, OPNA_ST_TIMER_A);
            if ((o->timer_ctrl & 0xC0) == 0x80) o->csm_pending = 1;     /* CSM: key channel 3 on */
        }
    }
    if (o->timer_ctrl & 2) {
        if (++o->tb_pre >= 16) {
            o->tb_pre = 0;
            if (--o->tb_left == 0) {
                o->tb_left = (uint16_t)(256 - o->tb_value);
                if (o->timer_ctrl & 8) set_flag(o, OPNA_ST_TIMER_B);
            }
        }
    }
}

/* ------------------------------------------------------------------ one FM sample */

static void fifo_push(Opna *o, int32_t l, int32_t r) {
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    if (o->fifo_len == OPNA_FIFO_FRAMES) {
        o->fifo_head = (o->fifo_head + 1) % OPNA_FIFO_FRAMES;
        o->fifo_len--;
        o->fifo_dropped++;
    }
    uint32_t at = (o->fifo_head + o->fifo_len) % OPNA_FIFO_FRAMES;
    o->fifo[at][0] = (int16_t)l;
    o->fifo[at][1] = (int16_t)r;
    o->fifo_len++;
}

static void sample_tick(Opna *o) {
    o->samples++;
    timers_sample(o);
    adpcm_brdy(o);
    int32_t al, ar;
    if (!o->synth) {
        adpcm_sample(o, &al, &ar);              /* playback still moves on: EOS and PCMBSY depend on it */
        return;
    }
    /* LFO and the envelope clock */
    if (lfo_on(o) && ++o->lfo_div >= lfo_divider[o->lfo & 7]) {
        o->lfo_div = 0;
        o->lfo_cnt = (o->lfo_cnt + 1) & 127;
    }
    int eg_tick = 0;
    if (++o->eg_div >= 3) {
        o->eg_div = 0;
        if (++o->eg_cnt >= 4096) o->eg_cnt = 1;
        eg_tick = 1;
    }
    if (o->csm_pending) {
        for (int i = 0; i < 4; i++) { o->ch[2].op[i].key_csm = 1; op_key(o, 2, i); }
    }
    unsigned am = am_now(o);
    int32_t fl = 0, fr = 0;
    int n = channels(o);
    for (int c = 0; c < n; c++) {
        int v = fm_channel(o, c, eg_tick, am);
        if (o->chip == OPNA_YM2608) {
            if (o->ch[c].pan & 0x80) fl += v;
            if (o->ch[c].pan & 0x40) fr += v;
        } else {
            fl += v;
            fr += v;
        }
    }
    if (o->csm_pending) {
        o->csm_pending = 0;
        for (int i = 0; i < 4; i++) { o->ch[2].op[i].key_csm = 0; op_key(o, 2, i); }
    }
    /* SSG at its own rate, box-filtered down to this sample */
    o->ssg_acc += o->sample_clocks;
    int32_t ssum = 0, sn = 0;
    while (o->ssg_acc >= o->ssg_clocks) {
        o->ssg_acc -= o->ssg_clocks;
        ssum += ssg_tick(o);
        sn++;
    }
    int32_t ssg = sn ? ssum / sn : o->last_ssg;
    o->last_ssg = ssg;
    int32_t ssg_out = ssg;
    if (o->ssg_dc_block) {
        /* one-pole high-pass at about 9 Hz (55466 Hz / 2^10 / 2 pi): y = x - x' + y' * (1 - 2^-10), in 1/256 */
        int32_t x = ssg << 8;
        int32_t y = x - o->ssg_hp_x + o->ssg_hp_y - (o->ssg_hp_y >> 10);
        o->ssg_hp_x = x;
        o->ssg_hp_y = y;
        ssg_out = y >> 8;
    }
    int32_t rl = 0, rr = 0;
    if (o->chip == OPNA_YM2608) {
        rhythm_sample(o, &rl, &rr);
        adpcm_sample(o, &al, &ar);
    } else {
        al = ar = 0;
    }
    o->last_fm[0] = fl;
    o->last_fm[1] = fr;
    o->last_rhythm[0] = rl;
    o->last_rhythm[1] = rr;
    o->last_adpcm[0] = al;
    o->last_adpcm[1] = ar;
    int32_t l = (fl * o->gain_fm + ssg_out * o->gain_ssg + rl * o->gain_rhythm + al * o->gain_adpcm) / 256;
    int32_t r = (fr * o->gain_fm + ssg_out * o->gain_ssg + rr * o->gain_rhythm + ar * o->gain_adpcm) / 256;
    fifo_push(o, l, r);
}

static void advance_clocks(Opna *o, uint64_t clocks) {
    while (clocks) {
        uint32_t to_next = o->sample_clocks - o->sample_pos;
        if (clocks < to_next) {
            o->sample_pos += (uint32_t)clocks;
            o->now += clocks;
            break;
        }
        o->now += to_next;
        clocks -= to_next;
        o->sample_pos = 0;
        sample_tick(o);
    }
    adpcm_brdy(o);
}

/* ------------------------------------------------------------------ interface */

static void reset_registers(Opna *o) {
    memset(o->addr, 0, sizeof o->addr);
    memset(o->fm_reg, 0, sizeof o->fm_reg);
    memset(o->ch, 0, sizeof o->ch);
    memset(&o->ssg, 0, sizeof o->ssg);
    memset(&o->adpcm, 0, sizeof o->adpcm);
    o->bank1_seen = 0;
    o->prescaler_sel = 2;
    o->busy_until = 0;
    o->ta_value = 0;
    o->tb_value = 0;
    o->timer_ctrl = 0;
    o->ta_left = 1024;
    o->tb_left = 256;
    o->tb_pre = 0;
    o->flags = 0;
    o->irq_enable = 0x1F;
    o->flag_mask = 0;
    o->fnum_latch = 0;
    o->ch3_latch = 0;
    memset(o->ch3_fnum, 0, sizeof o->ch3_fnum);
    memset(o->ch3_block, 0, sizeof o->ch3_block);
    o->csm_pending = 0;
    o->six_channels = 0;
    o->lfo = 0;
    o->lfo_cnt = 0;
    o->lfo_div = 0;
    o->eg_div = 0;
    o->eg_cnt = 1;
    for (int c = 0; c < 6; c++) {
        o->ch[c].pan = o->chip == OPNA_YM2608 ? 0xC0 : 0;
        for (int i = 0; i < 4; i++) {
            o->ch[c].op[i].att = 1023;
            o->ch[c].op[i].eg_state = OPNA_EG_RELEASE;
        }
    }
    o->ssg.lfsr = 1;
    o->ssg.port_in[0] = o->ssg.port_in[1] = 0xFF;
    ssg_env_restart(&o->ssg);                   /* as if shape 0 had been written: a ramp down, then silence */
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        o->rhythm[i].playing = 0;
        o->rhythm[i].pos = 0;
        o->rhythm[i].level = 0;
    }
    o->rhythm_tl = 0;
    adpcm_decoder_reset(&o->adpcm);
    o->adpcm.limit = OPNA_ADPCM_RAM;
    o->ssg_hp_x = o->ssg_hp_y = 0;
    o->last_ssg = 0;
    update_clocks(o);
    if (o->irq) {
        o->irq = 0;
        if (o->irq_cb) o->irq_cb(o->irq_ctx, 0);
    }
}

void opna_init(Opna *o, int chip, uint32_t clock_hz) {
    memset(o, 0, sizeof *o);
    o->chip = chip == OPNA_YM2608 ? OPNA_YM2608 : OPNA_YM2203;
    o->clock = clock_hz ? clock_hz : (o->chip == OPNA_YM2608 ? 7987200 : 3993600);
    build_tables(o);
    o->busy_clocks = 83;                        /* the manuals' wait after a data write (unverified as a duration) */
    o->synth = 1;
    o->ssg_dc_block = 1;
    o->gain_fm = 256;
    o->gain_ssg = 256;
    o->gain_rhythm = 128;
    o->gain_adpcm = 128;
    o->board_base = 0x188;
    o->board_id = 4;
    o->board_irq = 12;
    reset_registers(o);
}

void opna_reset(Opna *o) {
    reset_registers(o);
    o->fifo_head = o->fifo_len = 0;
}

void opna_set_host_clock(Opna *o, uint32_t host_hz) {
    o->host_hz = host_hz;
    o->host_frac = 0;
}

void opna_set_irq_callback(Opna *o, void (*cb)(void *ctx, int level), void *ctx) {
    o->irq_cb = cb;
    o->irq_ctx = ctx;
}

void opna_set_port_input(Opna *o, int port, uint8_t value) {
    o->ssg.port_in[port & 1] = value;
}

void opna_set_mix(Opna *o, int fm, int ssg, int rhythm, int adpcm) {
    o->gain_fm = (int16_t)fm;
    o->gain_ssg = (int16_t)ssg;
    o->gain_rhythm = (int16_t)rhythm;
    o->gain_adpcm = (int16_t)adpcm;
}

int opna_irq(const Opna *o) { return o->irq; }

void opna_write_addr(Opna *o, int bank, uint8_t reg) {
    bank &= 1;
    if (bank && o->chip != OPNA_YM2608) return;
    o->addr[bank] = reg;
    o->bank1_seen = (uint8_t)bank;
    if (!bank && reg >= 0x2D && reg <= 0x2F) {
        /* The prescaler is chosen by writing the address alone (manual): two flip-flops, 2Dh sets one, 2Eh the
         * other, 2Fh clears both; 1/6 after reset, 1/3 after 2Eh, 1/2 after 2Fh. */
        if (reg == 0x2D) o->prescaler_sel |= 2;
        else if (reg == 0x2E) o->prescaler_sel |= 1;
        else o->prescaler_sel = 0;
        update_clocks(o);
    }
}

void opna_write_data(Opna *o, int bank, uint8_t v) {
    bank &= 1;
    if (bank && o->chip != OPNA_YM2608) return;
    unsigned r = o->addr[bank];
    o->fm_reg[bank][r] = v;
    if (bank == 0) {
        if (r < 0x10) { ssg_write(o, r, v); return; }
        if (r < 0x20) { if (o->chip == OPNA_YM2608) rhythm_write(o, r, v); }
        else if (r < 0x30) control_write(o, r, v);
        else fm_write(o, 0, r, v);
    } else {
        if (r <= 0x10) adpcm_write(o, r, v);
        else if (r >= 0x30) fm_write(o, 3, r, v);
    }
    if (o->busy_clocks) o->busy_until = o->now + o->busy_clocks;
}

uint8_t opna_read_status(Opna *o, int bank) {
    bank &= 1;
    if (bank && o->chip != OPNA_YM2608) return 0xFF;
    adpcm_brdy(o);
    uint8_t s = o->now < o->busy_until ? OPNA_ST_BUSY : 0;
    uint8_t f = o->flags & (uint8_t)~o->flag_mask;
    if (!bank) return s | (f & (OPNA_ST_TIMER_A | OPNA_ST_TIMER_B));
    if (adpcm_ready_level(o) && !(o->flag_mask & OPNA_ST_BRDY)) f |= OPNA_ST_BRDY;
    if (o->adpcm.playing) s |= OPNA_ST_PCMBSY;
    return s | (f & 0x1F);
}

uint8_t opna_read_data(Opna *o, int bank) {
    bank &= 1;
    if (bank && o->chip != OPNA_YM2608) return 0xFF;
    unsigned r = o->addr[bank];
    if (!bank) {
        if (r < 0x10) return ssg_read(o, r);
        if (r == 0xFF && o->chip == OPNA_YM2608) return 0x01;  /* the YM2608's ID */
        return 0x00;                            /* (unverified) */
    }
    if (r == 0x08) return adpcm_read_data(o);
    if (r == 0x0F) return 0x00;                 /* A/D conversion result: no input here */
    return 0x00;
}

int opna_advance(Opna *o, uint64_t ticks) {
    if (!o->host_hz) {
        advance_clocks(o, ticks);
        return o->irq;
    }
    /* convert in pieces so ticks * clock cannot overflow */
    while (ticks) {
        uint64_t part = ticks > 1000000000u ? 1000000000u : ticks;
        ticks -= part;
        o->host_frac += part * o->clock;
        uint64_t clocks = o->host_frac / o->host_hz;
        o->host_frac %= o->host_hz;
        advance_clocks(o, clocks);
    }
    return o->irq;
}

uint64_t opna_ticks_to_irq(const Opna *o) {
    uint64_t best = UINT64_MAX;
    uint8_t reach = (uint8_t)~o->flag_mask & (o->chip == OPNA_YM2608 ? o->irq_enable : 0x03);
    /* samples until each timer's next overflow */
    if ((o->timer_ctrl & 5) == 5 && (reach & OPNA_ST_TIMER_A) && !(o->flags & OPNA_ST_TIMER_A)) {
        uint64_t s = o->ta_left;
        if (s < best) best = s;
    }
    if ((o->timer_ctrl & 10) == 10 && (reach & OPNA_ST_TIMER_B) && !(o->flags & OPNA_ST_TIMER_B)) {
        uint64_t s = (uint64_t)(o->tb_left - 1) * 16 + (16 - o->tb_pre);
        if (s < best) best = s;
    }
    if (best == UINT64_MAX) return best;
    uint64_t clocks = (best - 1) * o->sample_clocks + (o->sample_clocks - o->sample_pos);
    if (!o->host_hz) return clocks;
    uint64_t need = clocks * o->host_hz;
    need = need > o->host_frac ? need - o->host_frac : 0;
    return (need + o->clock - 1) / o->clock;
}

size_t opna_take(Opna *o, int16_t *stereo, size_t max_frames) {
    size_t n = 0;
    while (n < max_frames && o->fifo_len) {
        stereo[2 * n] = o->fifo[o->fifo_head][0];
        stereo[2 * n + 1] = o->fifo[o->fifo_head][1];
        o->fifo_head = (o->fifo_head + 1) % OPNA_FIFO_FRAMES;
        o->fifo_len--;
        n++;
    }
    return n;
}

void opna_render(Opna *o, int16_t *stereo, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (!o->fifo_len) {
            uint8_t synth = o->synth;
            if (!synth) {                       /* time still passes, the output is silence */
                advance_clocks(o, o->sample_clocks - o->sample_pos);
                stereo[2 * done] = stereo[2 * done + 1] = 0;
                done++;
                continue;
            }
            advance_clocks(o, o->sample_clocks - o->sample_pos);
        }
        done += opna_take(o, stereo + 2 * done, frames - done);
    }
}

/* ------------------------------------------------------------------ resampler */

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 60; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

void opna_resampler_init(OpnaResampler *r, uint32_t in_num, uint32_t in_den, uint32_t out_rate) {
    memset(r, 0, sizeof *r);
    r->in_num = in_num ? in_num : 1;
    r->in_den = in_den ? in_den : 1;
    r->out_rate = out_rate ? out_rate : 1;
    /* cutoff (cycles per input sample): 0.45 of the lower of the two rates */
    double in_rate = (double)r->in_num / (double)r->in_den;
    double low = in_rate < r->out_rate ? in_rate : (double)r->out_rate;
    double fc = 0.45 * low / in_rate;
    const double beta = 7.0, half = OPNA_RS_TAPS / 2;
    double i0b = bessel_i0(beta);
    for (int p = 0; p <= OPNA_RS_PHASES; p++) {
        double mu = (double)p / OPNA_RS_PHASES, h[OPNA_RS_TAPS], sum = 0.0;
        for (int j = 0; j < OPNA_RS_TAPS; j++) {
            double d = (half - 1 - j) + mu;     /* output time minus the tap's time, in input samples */
            double x = 2.0 * fc * d;
            double sinc = (x > -1e-12 && x < 1e-12) ? 1.0 : m_sin(M_PI_D * x) / (M_PI_D * x);
            double t = d / half;
            double w = (t <= -1.0 || t >= 1.0) ? 0.0 : bessel_i0(beta * m_sqrt(1.0 - t * t)) / i0b;
            h[j] = 2.0 * fc * sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < OPNA_RS_TAPS; j++) {
            double c = h[j] / sum * 32768.0;
            r->coef[p][j] = (int32_t)(c >= 0 ? c + 0.5 : c - 0.5);
        }
    }
    r->unit = (uint64_t)r->in_den * r->out_rate;     /* one input sample, in units of frac */
}

static int16_t rs_dot(const OpnaResampler *r, int ch, unsigned p, uint32_t w16) {
    const int16_t *h = &r->hist[ch][r->hpos];
    int64_t a = 0, b = 0;
    for (int j = 0; j < OPNA_RS_TAPS; j++) {
        a += (int64_t)h[j] * r->coef[p][j];
        b += (int64_t)h[j] * r->coef[p + 1][j];
    }
    int64_t v = (a * (int64_t)(65536 - w16) + b * (int64_t)w16) >> 31;     /* Q15 * Q16 */
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

size_t opna_resample(OpnaResampler *r, const int16_t *in, size_t *in_frames, int16_t *out, size_t out_max) {
    size_t used = 0, made = 0, avail = *in_frames;
    while (made < out_max) {
        while (r->frac >= r->unit) {
            if (used == avail) goto done;
            for (int c = 0; c < 2; c++) {
                r->hist[c][r->hpos] = in[2 * used + c];
                r->hist[c][r->hpos + OPNA_RS_TAPS] = in[2 * used + c];
            }
            r->hpos = (r->hpos + 1) % OPNA_RS_TAPS;
            used++;
            r->frac -= r->unit;
        }
        uint64_t ph = r->frac * OPNA_RS_PHASES;
        unsigned p = (unsigned)(ph / r->unit);
        uint32_t w16 = (uint32_t)(((ph % r->unit) << 16) / r->unit);
        out[2 * made] = rs_dot(r, 0, p, w16);
        out[2 * made + 1] = rs_dot(r, 1, p, w16);
        made++;
        r->frac += r->in_num;
    }
done:
    *in_frames = used;
    return made;
}

void opna_render_resampled(Opna *o, OpnaResampler *r, int16_t *stereo, size_t frames) {
    size_t made = 0;
    while (made < frames) {
        size_t none = 0;                        /* outputs that need no new input */
        made += opna_resample(r, NULL, &none, stereo + 2 * made, frames - made);
        if (made >= frames) break;
        int16_t f[2];                           /* then exactly one more chip frame */
        opna_render(o, f, 1);
        size_t one = 1;
        made += opna_resample(r, f, &one, stereo + 2 * made, frames - made);
    }
}

/* ------------------------------------------------------------------ PC-98 boards */

void opna_board_init(Opna *o, int board, int irq) {
    if (board == OPNA_BOARD_86) opna_init(o, OPNA_YM2608, 7987200);
    else opna_init(o, OPNA_YM2203, 3993600);
    o->board_base = 0x188;
    o->board_a460 = 0;
    o->pcm_volume = o->pcm_ctrl = o->pcm_mode = 0;
    o->pcm_bytes = 0;
    o->board_irq = (uint8_t)irq;
    /* port A bits 7-6: the interrupt jumper as the drivers decode it (00 INT0/IRQ 3, 01 INT6/IRQ 13, 10 INT41/IRQ 10,
     * 11 INT5/IRQ 12); bits 5-0: the joystick, nothing pressed */
    uint8_t sel = irq == 3 ? 0x00 : irq == 13 ? 0x40 : irq == 10 ? 0x80 : 0xC0;
    opna_set_port_input(o, 0, (uint8_t)(sel | 0x3F));
}

int opna_board_in(Opna *o, uint16_t port) {
    uint16_t b = o->board_base;
    if (port == b) return opna_read_status(o, 0);
    if (port == b + 2) return opna_read_data(o, 0);
    if (o->chip == OPNA_YM2608) {
        if (port == 0xA460) return (o->board_id << 4) | (o->board_a460 & 0x03);
        /* The PCM FIFO's registers. PMD86's interrupt entry reads A468h first and, while bit 4 is set, serves the FIFO and reads
         * again before it looks at the FM chip's timers: bit 4 is the FIFO's interrupt flag, and this model never raises it, so
         * the driver goes on to the timers (answered FFh as an unhandled port it never left that loop). The other bits read back
         * as written: the driver changes single bits with read, modify, write. */
        if (port == 0xA466) return o->pcm_volume;
        if (port == 0xA468) return (uint8_t)(o->pcm_ctrl & ~0x10u);
        if (port == 0xA46A) return o->pcm_mode;
        if (port == 0xA46C) return 0;
        int ext = o->board_a460 & 1;            /* the second bank only answers in OPNA mode */
        if (port == b + 4) return ext ? opna_read_status(o, 1) : 0xFF;
        if (port == b + 6) return ext ? opna_read_data(o, 1) : 0xFF;
    }
    return -1;
}

int opna_board_out(Opna *o, uint16_t port, uint8_t v) {
    uint16_t b = o->board_base;
    if (port == b) { opna_write_addr(o, 0, v); return 0; }
    if (port == b + 2) { opna_write_data(o, 0, v); return 0; }
    if (o->chip == OPNA_YM2608) {
        if (port == 0xA460) { o->board_a460 = v & 0x03; return 0; }
        if (port == 0xA466) { o->pcm_volume = v; return 0; }
        if (port == 0xA468) { o->pcm_ctrl = v; return 0; }
        if (port == 0xA46A) { o->pcm_mode = v; return 0; }
        if (port == 0xA46C) { o->pcm_bytes++; return 0; }          /* a sample byte: taken and dropped (nothing is played) */
        int ext = o->board_a460 & 1;
        if (port == b + 4) { if (ext) opna_write_addr(o, 1, v); return 0; }
        if (port == b + 6) { if (ext) opna_write_data(o, 1, v); return 0; }
    }
    return -1;
}
