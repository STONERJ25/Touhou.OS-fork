/*
 * opna: a software model of Yamaha's YM2203 ("OPN") and YM2608 ("OPNA"), the FM sound chips of NEC's PC-9801-26K and
 * PC-9801-86 boards, written from the chips' application manuals and our own understanding (docs/PC98_SOUND.md lists
 * what is modelled, what is approximated and what is unverified). Freestanding like the rest of libpc98: <stdint.h>,
 * <stddef.h>, memset and memcpy; nothing is allocated (the structure holds the 256 KiB ADPCM RAM, so allocate it or make
 * it static, do not put it on a stack). The sine, exponent and level tables are computed by opna_init() from their
 * definitions with our own small series for sin, log2 and exp2 (no libm, no tables pasted from anywhere).
 *
 * What a program sees, by port (the board decides the addresses; opna_board_* below does the PC-98 decoding):
 *
 *      address port A1=0 (188h)   write: register number of bank 0            read: status 0 (BUSY, FLAG B, FLAG A)
 *      data port    A1=0 (18Ah)   write: value for bank 0                     read: SSG registers 00h-0Fh, ID (FFh)
 *      address port A1=1 (18Ch)   write: register number of bank 1 (YM2608)   read: status 1 (+ PCMBSY, ZERO, BRDY, EOS)
 *      data port    A1=1 (18Eh)   write: value for bank 1                     read: ADPCM memory data (08h), PCM (0Fh)
 *
 * Time: the chip runs on its master clock (phiM: 3.9936 MHz on the -26K, 7.9872 MHz on the -86). opna_advance() moves
 * the chip forward; the unit is whatever opna_set_host_clock() said (CPU cycles, typically), master clocks by default.
 * Each FM sample period (72 phiM on the YM2203, 144 on the YM2608, both 55466 Hz on the PC-98) the timers count, the
 * envelope and phase generators step, and, when synthesis is on, one stereo frame goes into a FIFO the host drains with
 * opna_take(). A standalone player that has no CPU to keep pace with calls opna_render(), which advances the chip itself.
 *
 *      static Opna chip;
 *      opna_init(&chip, OPNA_YM2608, 7987200);
 *      opna_set_host_clock(&chip, 8000000);             // opna_advance() counts 8 MHz CPU cycles
 *      opna_write_addr(&chip, 0, 0x27); opna_write_data(&chip, 0, 0x3F);
 *      int irq = opna_advance(&chip, cycles);           // the interrupt line after `cycles`
 *      size_t n = opna_take(&chip, frames, 1024);       // the audio those cycles produced (55466 Hz stereo)
 */
#ifndef GAP_OPNA_H
#define GAP_OPNA_H

#include <stddef.h>
#include <stdint.h>

enum { OPNA_YM2203 = 0, OPNA_YM2608 = 1 };

/* Status bits (status 0 has BUSY and the two timer flags; status 1, YM2608 only, has all of them). */
#define OPNA_ST_TIMER_A     0x01
#define OPNA_ST_TIMER_B     0x02
#define OPNA_ST_EOS         0x04    /* ADPCM: end of sample (playback or memory access reached the stop address) */
#define OPNA_ST_BRDY        0x08    /* ADPCM: buffer ready (the next byte may be written or read) */
#define OPNA_ST_ZERO        0x10    /* ADPCM: zero (not modelled, never set) */
#define OPNA_ST_PCMBSY      0x20    /* ADPCM: playback from memory in progress */
#define OPNA_ST_BUSY        0x80

#define OPNA_FIFO_FRAMES    4096    /* about 74 ms at 55466 Hz */
#define OPNA_ADPCM_RAM      (256u * 1024u)  /* the -86 board's sample RAM */
#define OPNA_RHYTHMS        6       /* bass drum, snare, top cymbal, hi-hat, tom, rim shot */

/* One FM operator ("slot"). Register fields are kept decoded; att is the envelope generator's 10-bit attenuation
 * (0 = full level, 1023 = silent, 0.09375 dB per step). */
typedef struct {
    uint8_t dt, mul;        /* 30h: detune 0-7 (bit 2 = minus), multiple 0-15 (0 = one half) */
    uint8_t tl;             /* 40h: total level 0-127 (0.75 dB per step) */
    uint8_t ks, ar;         /* 50h: key scale 0-3, attack rate 0-31 */
    uint8_t am, dr;         /* 60h: amplitude modulation on (YM2608), decay rate 0-31 */
    uint8_t sr;             /* 70h: sustain rate 0-31 */
    uint8_t sl, rr;         /* 80h: sustain level 0-15, release rate 0-15 */
    uint8_t ssgeg;          /* 90h: SSG-type envelope (bit 3 on, 2 attack, 1 alternate, 0 hold) */
    uint8_t key_reg;        /* key on from register 28h */
    uint8_t key_csm;        /* key on from the CSM mode (timer A), for one sample */
    uint8_t keyed;          /* the key as the envelope generator saw it last */
    uint8_t eg_state;       /* OPNA_EG_* */
    uint8_t ssg_inv;        /* SSG-type envelope: the output is currently inverted */
    uint16_t att;           /* 0..1023 */
    uint32_t phase;         /* 20-bit phase accumulator; the top 10 bits index the sine */
    int16_t out;            /* the latest output (14-bit signed), what the next operator is modulated by */
} OpnaOp;

enum { OPNA_EG_ATTACK = 0, OPNA_EG_DECAY, OPNA_EG_SUSTAIN, OPNA_EG_RELEASE };

typedef struct {
    OpnaOp op[4];           /* S1, S2, S3, S4 (register offsets +0, +8, +4, +Ch) */
    uint16_t fnum;          /* A0h-A2h / A4h-A6h: F-number 0-2047 */
    uint8_t block;          /* octave 0-7 */
    uint8_t alg, fb;        /* B0h-B2h: algorithm 0-7, feedback 0-7 */
    uint8_t pan;            /* B4h-B6h bits 7-6: L, R (YM2608; the YM2203 is mono) */
    uint8_t ams, pms;       /* B4h-B6h: AM sensitivity 0-3, PM sensitivity 0-7 */
    int16_t fb_mem[2];      /* S1's two previous outputs (self feedback) */
} OpnaChannel;

typedef struct {
    uint8_t reg[16];        /* 00h-0Fh as written (all 8 bits read back) */
    uint16_t tone_cnt[3];   /* tone counters, in SSG ticks (phiSSG / 8) */
    uint8_t tone_out[3];    /* square wave outputs */
    uint8_t noise_cnt, noise_half;
    uint32_t lfsr;          /* 17-bit noise shift register */
    uint16_t env_cnt;       /* envelope step counter */
    uint8_t env_pos;        /* 0..31 within the current ramp */
    uint8_t env_attack;     /* ramp direction: 1 = up */
    uint8_t env_hold;       /* the envelope stopped */
    uint8_t env_level;      /* 0..31 */
    uint8_t port_in[2];     /* what the I/O pins of ports A and B read when they are inputs */
} OpnaSsg;

typedef struct {
    const int16_t *pcm;     /* host supplied (opna_set_rhythm_sample), not copied */
    uint32_t count;
    uint32_t rate;          /* Hz of the supplied PCM */
    uint64_t pos;           /* 32.32 fixed point, in samples of the drum */
    uint64_t step;          /* per FM sample */
    uint8_t playing;
    uint8_t level;          /* 18h-1Dh: bits 7-6 L, R; bits 4-0 instrument level */
} OpnaRhythm;

typedef struct {
    uint8_t reg[17];        /* bank 1 00h-10h */
    uint8_t mode;           /* OPNA_ADM_*: what the data register (08h) does now */
    uint8_t dummy;          /* memory read: dummy reads left before data comes out */
    uint8_t first;          /* memory write: the next write loads the address from the start register */
    uint8_t playing;
    uint8_t nibble_hi;      /* playback: the next nibble is the high one of its byte */
    uint32_t addr;          /* memory access / playback pointer, in bytes */
    uint32_t end;           /* last byte (inclusive) of the current access, from the stop register */
    uint32_t limit;         /* wrap-around point, from the limit register */
    uint32_t pos;           /* playback: 16-bit fraction between two decoded samples, advanced by DELTA-N */
    int32_t x, prev;        /* the decoder's output and the previous one (interpolated between) */
    int32_t step;           /* the decoder's step size, 127..24576 */
    uint64_t ready_at;      /* memory access: master clock when the current byte is done (BRDY comes back) */
    uint8_t brdy_pending;   /* a byte is in progress: BRDY rises at ready_at */
    uint8_t read_latch;
    uint32_t mem_writes, mem_reads;
    uint32_t early_writes;  /* data written while BRDY was still low (kept anyway; for debugging drivers) */
} OpnaAdpcm;

typedef struct Opna {
    int chip;               /* OPNA_YM2203 or OPNA_YM2608 */
    uint32_t clock;         /* phiM, Hz */
    uint8_t addr[2];        /* the register number latched for bank 0 / bank 1 */
    uint8_t bank1_seen;     /* the last address write went to bank 1 (some data reads depend on it) */
    uint8_t fm_reg[2][256]; /* every FM/control register as written (for debugging and save states) */

    /* clocks */
    uint8_t prescaler_sel;  /* two flip-flops set by 2Dh/2Eh, cleared by 2Fh: 2 = 1/6 (reset), 3 = 1/3, 0/1 = 1/2 */
    uint32_t sample_clocks; /* phiM per FM sample: prescaler * 12 (YM2203) or * 24 (YM2608) */
    uint32_t ssg_clocks;    /* phiM per SSG tick (tone counter step) */
    uint64_t now;           /* master clocks since opna_init */
    uint32_t sample_pos;    /* phiM into the current FM sample */
    uint32_t ssg_acc;
    uint32_t host_hz;       /* unit of opna_advance; 0 = master clocks */
    uint64_t host_frac;
    uint64_t samples;       /* FM samples since opna_init */

    /* busy flag */
    uint64_t busy_until;
    uint32_t busy_clocks;   /* how long a data write keeps BUSY set, in phiM (0 = never busy) */

    /* timers */
    uint16_t ta_value;      /* NA, 10 bits */
    uint8_t tb_value;       /* NB */
    uint8_t timer_ctrl;     /* 27h as written, without the reset strobes */
    uint16_t ta_left;       /* samples until timer A overflows */
    uint16_t tb_left;       /* timer B ticks (16 samples each) until it overflows */
    uint8_t tb_pre;         /* samples into the current timer B tick */
    uint8_t flags;          /* OPNA_ST_TIMER_A | TIMER_B | EOS | BRDY | ZERO, latched */
    uint8_t irq_enable;     /* YM2608 29h bits 4-0 (1Fh after reset) */
    uint8_t flag_mask;      /* YM2608 110h bits 4-0: 1 = that flag is masked */
    uint8_t irq;            /* the interrupt line */
    uint32_t irq_count;     /* rising edges so far */
    void (*irq_cb)(void *ctx, int level);
    void *irq_ctx;

    /* FM */
    OpnaChannel ch[6];
    uint8_t fnum_latch;     /* A4h-A6h: block and F-number high bits, waiting for the low byte */
    uint8_t ch3_latch;      /* ACh-AEh, the same for the channel 3 special mode registers */
    uint16_t ch3_fnum[3];   /* channel 3 special mode: F-numbers of S1 (A9h), S2 (AAh), S3 (A8h); S4 uses A2h */
    uint8_t ch3_block[3];
    uint8_t csm_pending;
    uint8_t six_channels;   /* YM2608 29h bit 7 (SCH): channels 4-6 exist */
    uint8_t lfo;            /* 22h: bit 3 on, bits 2-0 frequency */
    uint8_t lfo_cnt;        /* 0..127 */
    uint8_t lfo_div;
    uint8_t eg_div;         /* the envelope generator steps every third sample */
    uint16_t eg_cnt;        /* 1..4095 */

    /* SSG, rhythm, ADPCM */
    OpnaSsg ssg;
    OpnaRhythm rhythm[OPNA_RHYTHMS];
    uint8_t rhythm_tl;      /* 11h: total level 0-63 */
    OpnaAdpcm adpcm;
    uint8_t adpcm_ram[OPNA_ADPCM_RAM];

    /* output */
    uint8_t synth;          /* 1: produce audio while advancing (default); 0: timers and flags only */
    uint8_t ssg_dc_block;   /* 1: high-pass the SSG output like the board's coupling capacitor (default) */
    int16_t gain_fm, gain_ssg, gain_rhythm, gain_adpcm;   /* 256 = 1.0 */
    int32_t ssg_hp_x, ssg_hp_y;     /* DC blocker state (in 1/256) */
    int32_t last_fm[2], last_ssg, last_rhythm[2], last_adpcm[2];  /* the latest frame's parts, for tests */
    int16_t fifo[OPNA_FIFO_FRAMES][2];
    uint32_t fifo_head, fifo_len;
    uint32_t fifo_dropped;  /* frames lost because nobody drained the FIFO */

    /* tables, built by opna_init */
    uint16_t logsin[256];   /* -log2(sin) of a quarter wave, 4.8 fixed point */
    uint16_t exptab[256];   /* 2^(-x/256) as 1024..2042 (an 11-bit mantissa) */
    uint16_t ssg_dac[32];   /* SSG output levels, 1.5 dB apart */
    uint16_t rhythm_gain[96];   /* 0.75 dB steps, Q15 */
    uint16_t pm_ratio[8][8];    /* LFO vibrato: frequency ratio - 1 for each PMS and quarter-wave step, Q16 */

    /* -86 board decoding (opna_board_*) */
    uint16_t board_base;    /* 188h */
    uint8_t board_a460;     /* A460h as last written: bit 0 = the YM2608's second bank answers */
    uint8_t board_id;       /* the upper nibble A460h reads (4 on the -86) */
    uint8_t board_irq;      /* the interrupt the board's jumper selects (3, 10, 12, 13) */
    /* -86 board, the separate PCM FIFO (ports A466h-A46Ch): the registers only, nothing is played and no interrupt is ever raised
     * (docs/PC98_SOUND.md, "The -86 board's PCM FIFO"): what PMD86 needs to get past its start-up */
    uint8_t pcm_volume;     /* A466h: PMD86 writes A0h | (15 - level) */
    uint8_t pcm_ctrl;       /* A468h as written: bit 7 play, 5 interrupt enable, 4 interrupt flag (0 clears, 1 arms), 3 FIFO reset, 2-0 rate */
    uint8_t pcm_mode;       /* A46Ah as written */
    uint32_t pcm_bytes;     /* bytes written to the FIFO data port A46Ch (tests and tools) */
} Opna;

/* ---- the chip */

/* Power-on: every register cleared the way the /IC pin leaves them (B4h-B6h = C0h: both speakers), the prescaler at
 * 1/6, the IRQ enables (29h) at 1Fh, timers stopped, synthesis on with the default mix. clock_hz is phiM. */
void opna_init(Opna *o, int chip, uint32_t clock_hz);
/* The /IC pin again: registers and the sound cleared; tables, host settings, rhythm samples and ADPCM RAM kept. */
void opna_reset(Opna *o);

void opna_write_addr(Opna *o, int bank, uint8_t reg);
void opna_write_data(Opna *o, int bank, uint8_t value);
uint8_t opna_read_status(Opna *o, int bank);
uint8_t opna_read_data(Opna *o, int bank);

/* The unit of opna_advance: host ticks per second (a CPU clock), or 0 for master clocks. */
void opna_set_host_clock(Opna *o, uint32_t host_hz);
/* Lets `ticks` host ticks pass. Returns the interrupt line (1 = asserted); irq_cb, when set, hears every change. */
int opna_advance(Opna *o, uint64_t ticks);
/* Host ticks until a timer sets a flag that reaches the interrupt line (UINT64_MAX when nothing would), for scheduling. */
uint64_t opna_ticks_to_irq(const Opna *o);
int opna_irq(const Opna *o);
void opna_set_irq_callback(Opna *o, void (*cb)(void *ctx, int level), void *ctx);

/* What the SSG's I/O pins read when the port is an input (port 0 = A, 1 = B). On the PC-98 boards port A bits 7-6 are
 * the interrupt jumper and bits 5-0 the joystick (0 = pressed); opna_board_init sets it. */
void opna_set_port_input(Opna *o, int port, uint8_t value);

/* The YM2608's six rhythm sounds live in its ROM; the host supplies them as 16-bit mono PCM at `rate` Hz
 * (index: 0 bass drum, 1 snare, 2 top cymbal, 3 hi-hat, 4 tom, 5 rim shot). The array is used in place, not copied. */
void opna_set_rhythm_sample(Opna *o, int index, const int16_t *pcm, uint32_t count, uint32_t rate);

/* ---- audio */

/* The rate of opna_take/opna_render: phiM / sample_clocks, as a fraction (55466.67 Hz = 7987200 / 144). */
uint32_t opna_rate_num(const Opna *o);
uint32_t opna_rate_den(const Opna *o);
/* Mix levels, 256 = 1.0: FM is the chips' own sum (one channel at full level is 8168), the others are scaled to sit
 * with it the way the boards roughly sound (approximate: docs/PC98_SOUND.md). */
void opna_set_mix(Opna *o, int fm, int ssg, int rhythm, int adpcm);
/* Drains up to max_frames stereo frames (L, R interleaved) that opna_advance produced. Returns the frames copied. */
size_t opna_take(Opna *o, int16_t *stereo, size_t max_frames);
/* Produces exactly `frames` frames at the native rate: what is buffered first, then by advancing the chip (timers and
 * all) sample by sample. For players without a CPU; a machine uses opna_advance + opna_take instead. */
void opna_render(Opna *o, int16_t *stereo, size_t frames);

/* A windowed-sinc resampler from the chip rate to the host's (44100, 48000, ...): 32 taps, 64 phases with linear
 * interpolation between them, integer arithmetic only. Works on any stereo 16-bit stream. */
#define OPNA_RS_TAPS    32
#define OPNA_RS_PHASES  64
typedef struct {
    uint64_t in_num, in_den;    /* input rate in_num / in_den Hz */
    uint32_t out_rate;
    uint64_t frac;              /* where the next output falls past the history's centre, in 1/unit input samples;
                                   each output adds in_num, each input taken subtracts unit */
    uint64_t unit;              /* in_den * out_rate: one input sample */
    int16_t hist[2][OPNA_RS_TAPS * 2];
    uint32_t hpos;              /* next history slot */
    uint32_t have;              /* frames of history filled (up to TAPS) */
    int32_t coef[OPNA_RS_PHASES + 1][OPNA_RS_TAPS];   /* Q15 */
} OpnaResampler;

void opna_resampler_init(OpnaResampler *r, uint32_t in_num, uint32_t in_den, uint32_t out_rate);
/* Converts *in_frames input frames into at most out_max output frames; on return *in_frames says how many inputs were
 * used (all of them unless the output was full). Returns the frames written. */
size_t opna_resample(OpnaResampler *r, const int16_t *in, size_t *in_frames, int16_t *out, size_t out_max);
/* opna_render at the resampler's output rate. */
void opna_render_resampled(Opna *o, OpnaResampler *r, int16_t *stereo, size_t frames);

/* ---- the PC-98 boards: port decoding for the machine (pc98.h's port_in/port_out hooks) */

enum { OPNA_BOARD_26K = 0, OPNA_BOARD_86 = 1 };
/* PC-9801-26K: a YM2203 at 3.9936 MHz on 188h/18Ah. PC-9801-86: a YM2608 at 7.9872 MHz on 188h-18Eh and A460h.
 * irq: the jumper (3 = INT0, 10 = INT41, 12 = INT5 (the factory setting), 13 = INT6); drivers read it from SSG port A. */
void opna_board_init(Opna *o, int board, int irq);
/* -1 when the port is not the board's; otherwise the byte read. */
int opna_board_in(Opna *o, uint16_t port);
/* -1 when the port is not the board's, else 0. */
int opna_board_out(Opna *o, uint16_t port, uint8_t value);

/* ---- for tests */
int opna_op_output(const Opna *o, uint32_t phase, int mod, unsigned att);   /* one operator lookup, as the chip does */
uint32_t opna_phase_inc(const Opna *o, unsigned fnum, unsigned block, unsigned dt, unsigned mul, int pm);

#endif
