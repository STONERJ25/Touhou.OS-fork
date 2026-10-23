/*
 * Intel High Definition Audio: controller, codec and the playback stream.
 *
 * Polled throughout (no interrupts, no MSI): commands go through the CORB/RIRB rings (or the immediate command registers
 * if the rings do not answer), the play position comes from the stream descriptor (LPIB, with the DMA position buffer as
 * a second opinion). The driver
 *   1. resets the controller (retrying once) and waits for the codecs to announce themselves in STATESTS,
 *   2. reads each codec's audio function group into an HdaGraph (hda_graph.h): widget types and capabilities,
 *      connection lists, pin default configuration and capabilities, amplifier and PCM capabilities,
 *   3. lets hda_graph.c choose the output pins and the converter path to each (pin <- mixers/selectors <- DAC),
 *   4. powers the widgets on the paths, selects connections, mutes every other mixer input, unmutes the amplifiers at 0 dB,
 *      switches on the pins (output enable, headphone amp, EAPD) as the jacks allow,
 *   5. loops one output stream (48 kHz, 16-bit, stereo) over a 256 KiB ring for the audio core,
 *   6. watches the jacks: headphones silence the speaker and the rear line out, like Linux's auto-mute.
 * Anything unexpected is logged once and the driver stays out of the way instead of making noise. Nothing here has run on
 * real hardware yet; /proc/audio prints everything the driver knows about the codec so a failure can be diagnosed.
 */
#include "audiodev.h"
#include "hda_graph.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "mm.h"
#include "task.h"
#include "cmdline.h"
#include "initcall.h"

/* controller registers */
#define R_GCAP      0x00
#define R_GCTL      0x08
#define R_STATESTS  0x0E
#define R_INTCTL    0x20
#define R_INTSTS    0x24
#define R_CORBLBASE 0x40
#define R_CORBUBASE 0x44
#define R_CORBWP    0x48
#define R_CORBRP    0x4A
#define R_CORBCTL   0x4C
#define R_CORBSIZE  0x4E
#define R_RIRBLBASE 0x50
#define R_RIRBUBASE 0x54
#define R_RIRBWP    0x58
#define R_RINTCNT   0x5A
#define R_RIRBCTL   0x5C
#define R_RIRBSTS   0x5D
#define R_RIRBSIZE  0x5E
#define R_ICOI      0x60                    /* immediate command output / input / status */
#define R_IRII      0x64
#define R_ICIS      0x68
#define R_DPLBASE   0x70
#define R_DPUBASE   0x74
#define R_SD0       0x80                    /* stream descriptors, 0x20 bytes each: input streams first, then output */
#define SD_CTL      0x00                    /* 24 bits: byte 0 flags, byte 2 = stream tag << 4 */
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define STREAM_TAG 1
#define RING_FRAMES 65536                   /* 1.4 s: the host can stall the guest for a long time and the hardware keeps going */
#define BDL_ENTRIES 16
#define STREAM_FORMAT 0x0011                /* 48 kHz, 16 bits, 2 channels */

typedef struct Hda {
    PciDev *pci;
    volatile u8 *mmio;
    u32 *corb;  u64 *rirb;
    u32 corb_n, rirb_n, corb_wp, rirb_rp;
    bool use_imm, tried_imm;                /* immediate command registers instead of the rings */
    int iss, oss;
    bool ok64;
    u8 *bdl;    paddr_t bdl_phys;
    u8 *ring;   paddr_t ring_phys;
    u32 *posbuf; paddr_t posbuf_phys;       /* DMA position buffer: the controller writes each stream's position here */
    bool posbuf_on, use_posbuf;
    u32 last_lpib, lpib_still;
    int codec;                              /* address of the codec in use */
    u8  afg;
    u32 vendor_id, subsys_id;
    HdaWidget *w;
    HdaGraph graph;
    HdaPlan plan;
    int sd;                                 /* offset of our output stream descriptor */
    int timeouts;
    bool dead;                              /* the codec stopped answering: give up quietly */
    bool pins_written;
    AudioBackend be;
} Hda;

static Hda g_hda;

/* ------------------------------------------------------------------ registers */

static inline u8  rd8(Hda *h, u32 o)  { return *(volatile u8 *)(h->mmio + o); }
static inline u16 rd16(Hda *h, u32 o) { return *(volatile u16 *)(h->mmio + o); }
static inline u32 rd32(Hda *h, u32 o) { return *(volatile u32 *)(h->mmio + o); }
static inline void wr8(Hda *h, u32 o, u8 v)   { *(volatile u8 *)(h->mmio + o) = v; }
static inline void wr16(Hda *h, u32 o, u16 v) { *(volatile u16 *)(h->mmio + o) = v; }
static inline void wr32(Hda *h, u32 o, u32 v) { *(volatile u32 *)(h->mmio + o) = v; }

/* Spins until (reg & mask) == want, at most ms milliseconds. */
static bool wait_bits8(Hda *h, u32 reg, u8 mask, u8 want, u32 ms) {
    for (u32 i = 0; i < ms * 20; i++) {
        if ((rd8(h, reg) & mask) == want) return true;
        udelay(50);
    }
    return false;
}

static bool wait_bits32(Hda *h, u32 reg, u32 mask, u32 want, u32 ms) {
    for (u32 i = 0; i < ms * 20; i++) {
        if ((rd32(h, reg) & mask) == want) return true;
        udelay(50);
    }
    return false;
}

/* ------------------------------------------------------------------ commands */

/* Through the CORB/RIRB rings. */
static int cmd_ring(Hda *h, u32 cmd, u32 *resp) {
    u32 wp = (h->corb_wp + 1) % h->corb_n;
    h->corb[wp] = cmd;
    barrier();
    h->corb_wp = wp;
    wr16(h, R_CORBWP, (u16)wp);
    for (u32 i = 0; i < 4000; i++) {                    /* up to about 100 ms */
        u32 rwp = rd16(h, R_RIRBWP) & 0xFF;
        while (h->rirb_rp != rwp % h->rirb_n) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_n;
            u64 e = h->rirb[h->rirb_rp];
            if ((e >> 32) & 0x10) continue;             /* unsolicited response: not ours */
            if (resp) *resp = (u32)e;
            wr8(h, R_RIRBSTS, 0x05);                    /* acknowledge: response interrupt flag and overrun (QEMU stops reading commands until then) */
            return 0;
        }
        udelay(25);
    }
    return -ETIMEDOUT;
}

/* Through the immediate command registers: one command at a time, no memory involved. */
static int cmd_immediate(Hda *h, u32 cmd, u32 *resp) {
    int i;
    for (i = 0; i < 1000 && (rd16(h, R_ICIS) & 1); i++) udelay(10);     /* busy */
    if (i == 1000) return -EBUSY;
    wr16(h, R_ICIS, (u16)(rd16(h, R_ICIS) | 2));                        /* clear "result valid" (write one) */
    wr32(h, R_ICOI, cmd);
    wr16(h, R_ICIS, (u16)(rd16(h, R_ICIS) | 1));                        /* go */
    for (i = 0; i < 2000; i++) {
        if ((rd16(h, R_ICIS) & 3) == 2) { if (resp) *resp = rd32(h, R_IRII); return 0; }
        udelay(10);
    }
    return -ETIMEDOUT;
}

static int cmd_raw(Hda *h, u32 cmd, u32 *resp) {
    if (h->dead) return -EIO;
    int rc = h->use_imm ? cmd_immediate(h, cmd, resp) : cmd_ring(h, cmd, resp);
    if (rc && !h->use_imm && !h->tried_imm && h->timeouts == 0) {       /* the rings are silent: try the immediate registers once */
        h->tried_imm = true;
        if (cmd_immediate(h, cmd, resp) == 0) {
            klog("Nitori", "hda: the CORB/RIRB rings did not answer; using the immediate command registers");
            h->use_imm = true;
            h->timeouts = 0;
            return 0;
        }
    }
    if (rc) {
        if (++h->timeouts >= 3) { h->dead = true; klog("Nitori", "hda: the codec stopped answering commands"); }
    } else {
        h->timeouts = 0;
    }
    return rc;
}

/* 12-bit verb with an 8-bit payload (get/set verbs 0xFxx and 0x7xx). */
static u32 verb8(Hda *h, u32 nid, u32 verb, u32 payload) {
    u32 r = 0;
    if (cmd_raw(h, ((u32)h->codec << 28) | (nid << 20) | (verb << 8) | (payload & 0xFF), &r)) return 0;
    return r;
}

/* 4-bit verb with a 16-bit payload (amp gain/mute, converter format). */
static u32 verb16(Hda *h, u32 nid, u32 verb, u32 payload) {
    u32 r = 0;
    if (cmd_raw(h, ((u32)h->codec << 28) | (nid << 20) | (verb << 16) | (payload & 0xFFFF), &r)) return 0;
    return r;
}

static u32 param(Hda *h, u32 nid, u32 p) { return verb8(h, nid, 0xF00, p); }

/* ------------------------------------------------------------------ controller */

static bool controller_reset(Hda *h) {
    wr32(h, R_GCTL, rd32(h, R_GCTL) & ~1u);             /* enter reset */
    if (!wait_bits32(h, R_GCTL, 1, 0, 100)) { klog("Nitori", "hda: the controller would not enter reset"); return false; }
    task_sleep_ms(2);                                   /* the specification wants at least 100 microseconds; Linux waits about a millisecond */
    wr32(h, R_GCTL, rd32(h, R_GCTL) | 1u);              /* leave reset */
    if (!wait_bits32(h, R_GCTL, 1, 1, 200)) { klog("Nitori", "hda: the controller would not leave reset"); return false; }
    task_sleep_ms(2);                                   /* codecs announce themselves within 521 microseconds of the link coming up */
    wr32(h, R_INTCTL, 0);                               /* polled: no interrupts */
    wr32(h, R_INTSTS, 0xC00000FFu);
    return true;
}

/* Resets and waits for at least one codec; once more if nobody answered (some firmware leaves the link in a odd state). */
static u16 controller_up(Hda *h) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!controller_reset(h)) continue;
        u16 present = 0;
        for (int i = 0; i < 1000 && !present; i++) {
            present = rd16(h, R_STATESTS) & 0x7FFF;
            if (!present) task_sleep_ms(1);
        }
        if (present) { wr16(h, R_STATESTS, present); return present; }
        klog("Nitori", "hda: no codec answered after the reset%s", attempt ? "" : "; trying once more");
    }
    return 0;
}

static bool cmd_rings_init(Hda *h) {
    paddr_t cp = 0, rp = 0;
    h->corb = dma_alloc(4096, &cp);
    h->rirb = dma_alloc(4096, &rp);
    if (!h->corb || !h->rirb) return false;

    u8 cs = rd8(h, R_CORBSIZE), rs = rd8(h, R_RIRBSIZE);
    h->corb_n = (cs & 0x40) ? 256 : (cs & 0x20) ? 16 : 2;       /* the capability bits say which sizes exist */
    h->rirb_n = (rs & 0x40) ? 256 : (rs & 0x20) ? 16 : 2;
    u8 want_cs = (u8)((cs & 0xFC) | (h->corb_n == 256 ? 2 : h->corb_n == 16 ? 1 : 0));
    u8 want_rs = (u8)((rs & 0xFC) | (h->rirb_n == 256 ? 2 : h->rirb_n == 16 ? 1 : 0));
    if (want_cs != cs) wr8(h, R_CORBSIZE, want_cs);             /* some controllers fix the size and complain about writes */
    if (want_rs != rs) wr8(h, R_RIRBSIZE, want_rs);

    wr8(h, R_CORBCTL, 0);                               /* DMA off while it is programmed */
    wr8(h, R_RIRBCTL, 0);
    wr32(h, R_CORBLBASE, (u32)cp);
    wr32(h, R_CORBUBASE, h->ok64 ? (u32)(cp >> 32) : 0);
    wr32(h, R_RIRBLBASE, (u32)rp);
    wr32(h, R_RIRBUBASE, h->ok64 ? (u32)(rp >> 32) : 0);

    wr16(h, R_CORBRP, 0x8000);                          /* reset the read pointer: set, wait for 1, clear, wait for 0 */
    for (int i = 0; i < 1000 && !(rd16(h, R_CORBRP) & 0x8000); i++) udelay(10);
    wr16(h, R_CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(h, R_CORBRP) & 0x8000); i++) udelay(10);
    wr16(h, R_CORBWP, 0);
    h->corb_wp = 0;

    wr16(h, R_RIRBWP, 0x8000);
    wr16(h, R_RINTCNT, 1);
    h->rirb_rp = 0;
    wr8(h, R_RIRBCTL, 0x03);                            /* response DMA on; the response-count flag is polled and acknowledged after every command */
    wr8(h, R_CORBCTL, 0x02);                            /* command DMA on */
    return true;
}

/* ------------------------------------------------------------------ codec graph */

static void read_connections(Hda *h, u32 nid, HdaWidget *w) {
    u32 lenp = param(h, nid, 0x0E);
    u32 len = lenp & 0x7F;
    bool longform = lenp & 0x80;
    u32 prev = 0;
    w->nconn = 0;
    if (len > 64) len = 64;
    u32 per = longform ? 2 : 4;
    for (u32 i = 0; i < len; i += per) {
        u32 r = verb8(h, nid, 0xF02, i);
        for (u32 k = 0; k < per && i + k < len; k++) {
            u32 e = longform ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
            u32 range = longform ? e & 0x8000 : e & 0x80;
            u32 n = longform ? e & 0x7FFF : e & 0x7F;
            if (range && prev) {                        /* "n" ends a range that starts after the previous entry */
                for (u32 x = prev + 1; x <= n && w->nconn < HDA_MAX_CONN; x++) w->conn[w->nconn++] = (u8)x;
            } else if (w->nconn < HDA_MAX_CONN) {
                w->conn[w->nconn++] = (u8)n;
            }
            prev = n;
        }
    }
}

static bool afg_to_d0(Hda *h, u32 afg) {
    verb8(h, afg, 0x705, 0);                            /* function group to D0 */
    for (int i = 0; i < 100; i++) {
        u32 r = verb8(h, afg, 0xF05, 0);
        if (((r >> 4) & 0xF) == 0) break;               /* the actual power state caught up with the requested one */
        task_sleep_ms(1);
    }
    task_sleep_ms(10);
    return !h->dead;
}

static bool parse_codec(Hda *h, int addr) {
    h->codec = addr;
    u32 vid = param(h, 0, 0x00);
    if (!vid || vid == 0xFFFFFFFF) { klog("Nitori", "hda: codec address %d does not answer (%08x)", addr, vid); return false; }
    u32 root = param(h, 0, 0x04);
    u32 fg0 = (root >> 16) & 0xFF, nfg = root & 0xFF;
    int afg = -1;
    for (u32 i = 0; i < nfg && i < 8; i++)
        if ((param(h, fg0 + i, 0x05) & 0xFF) == 1) { afg = (int)(fg0 + i); break; }
    if (afg < 0) { klog("Nitori", "hda: codec %08x has no audio function group (root %08x)", vid, root); return false; }
    u32 sub = param(h, (u32)afg, 0x04);
    h->afg = (u8)afg;
    h->vendor_id = vid;
    h->subsys_id = verb8(h, (u32)afg, 0xF20, 0);
    h->graph.first = (u8)((sub >> 16) & 0xFF);
    h->graph.count = (u8)(sub & 0xFF);
    if (!h->graph.count) return false;
    kfree(h->w);
    h->w = kzalloc(sizeof(HdaWidget) * h->graph.count);
    if (!h->w) return false;
    h->graph.w = h->w;

    afg_to_d0(h, (u32)afg);

    u32 afg_ampout = param(h, (u32)afg, 0x12), afg_ampin = param(h, (u32)afg, 0x0D), afg_pcm = param(h, (u32)afg, 0x0A);
    for (u32 i = 0; i < h->graph.count && !h->dead; i++) {
        u32 nid = h->graph.first + i;
        HdaWidget *w = &h->w[i];
        w->caps = param(h, nid, 0x09);
        w->type = (u8)HDA_WC_TYPE(w->caps);
        w->valid = true;
        w->ampout = (w->caps & HDA_WC_AMP_OVRD) ? param(h, nid, 0x12) : afg_ampout;
        w->ampin = (w->caps & HDA_WC_AMP_OVRD) ? param(h, nid, 0x0D) : afg_ampin;
        if (w->type == HDA_WT_OUT) w->pcm = (w->caps & HDA_WC_FMT_OVRD) ? param(h, nid, 0x0A) : afg_pcm;
        if (w->caps & HDA_WC_CONN_LIST) read_connections(h, nid, w);
        if (w->type == HDA_WT_PIN) {
            w->pincap = param(h, nid, 0x0C);
            w->pincfg = verb8(h, nid, 0xF1C, 0);
        }
    }
    return !h->dead;
}

/* ------------------------------------------------------------------ programming the paths */

static u32 amp_zero_db(u32 cap) { return cap & 0x7F; }  /* the offset field is the step that means 0 dB */
static bool amp_usable(u32 cap) { return (cap & 0x7FFFFFFF) || (cap >> 31); }

static void set_amp_out(Hda *h, u32 nid, u32 cap, bool unmute) {
    if (!amp_usable(cap)) return;
    u32 val = unmute ? amp_zero_db(cap) : ((cap >> 31) ? 0x80 : 0);
    verb16(h, nid, 0x3, 0xB000 | val);                  /* output amp, left and right */
}

static void set_amp_in(Hda *h, u32 nid, u32 cap, u32 index, bool unmute) {
    if (!amp_usable(cap)) return;
    u32 val = unmute ? amp_zero_db(cap) : ((cap >> 31) ? 0x80 : 0);
    verb16(h, nid, 0x3, 0x7000 | (index << 8) | val);   /* input amp, left and right, this connection */
}

static void power_up(Hda *h, u32 nid, const HdaWidget *w) {
    if (!(w->caps & HDA_WC_POWER)) return;              /* no power control: always on */
    verb8(h, nid, 0x705, 0);
    for (int i = 0; i < 100; i++) {
        u32 r = verb8(h, nid, 0xF05, 0);
        if (((r >> 4) & 0xF) == 0) break;
        task_sleep_ms(1);
    }
}

/* Powers every widget on every path, closes every mixer input, then selects the connections each path needs and opens
 * exactly those inputs: two pins may share a mixer, and one path must not close the input the other one uses. */
static void program_paths(Hda *h) {
    HdaPlan *pl = &h->plan;
    for (int i = 0; i < pl->npins; i++) {
        HdaPath *pa = &pl->pin[i].path;
        for (u32 k = 0; k < pa->n; k++) {
            const HdaWidget *w = hda_widget(&h->graph, pa->nid[k]);
            if (!w) continue;
            power_up(h, pa->nid[k], w);
            if (w->type == HDA_WT_MIXER && (w->caps & HDA_WC_AMP_IN))
                for (u32 j = 0; j < w->nconn; j++) set_amp_in(h, pa->nid[k], w->ampin, j, false);
        }
    }
    for (int i = 0; i < pl->npins; i++) {
        HdaPath *pa = &pl->pin[i].path;
        for (u32 k = 0; k < pa->n; k++) {
            const HdaWidget *w = hda_widget(&h->graph, pa->nid[k]);
            if (!w) continue;
            if (k + 1 < pa->n) {
                if (w->nconn > 1 && (w->type == HDA_WT_SEL || w->type == HDA_WT_PIN)) verb8(h, pa->nid[k], 0x701, pa->idx[k]);
                if (w->caps & HDA_WC_AMP_IN) set_amp_in(h, pa->nid[k], w->ampin, pa->idx[k], true);
            }
            if (w->caps & HDA_WC_AMP_OUT) set_amp_out(h, pa->nid[k], w->ampout, true);
        }
    }
}

static void pin_enable(Hda *h, HdaOutPin *p, bool on) {
    const HdaWidget *w = hda_widget(&h->graph, p->nid);
    if (!w) return;
    if (on) {
        u32 ctl = 0x40;                                 /* OUT_EN */
        if (p->dev == HDA_DEV_HEADPHONE && (w->pincap & HDA_PC_HP_DRIVE)) ctl |= 0x80;     /* HP_EN on headphone drive capable pins */
        verb8(h, p->nid, 0x707, ctl);
        if (w->pincap & HDA_PC_EAPD) verb8(h, p->nid, 0x70C, 0x02);   /* EAPD on */
        if (w->caps & HDA_WC_AMP_OUT) set_amp_out(h, p->nid, w->ampout, true);
    } else {
        verb8(h, p->nid, 0x707, 0);
        if (w->pincap & HDA_PC_EAPD) verb8(h, p->nid, 0x70C, 0x00);
    }
    p->want = on;
}

/* Reads the jacks and applies the auto-mute rules (hda_plan_automute); `initial` forces every pin to be written once. */
static void apply_jacks(Hda *h, bool initial) {
    HdaPlan *pl = &h->plan;
    bool before[HDA_MAX_PINS];
    for (int i = 0; i < pl->npins; i++) {
        HdaOutPin *p = &pl->pin[i];
        before[i] = p->want;
        if (!p->detect) continue;
        const HdaWidget *pw = hda_widget(&h->graph, p->nid);
        if (pw && (pw->pincap & (1u << 1))) verb8(h, p->nid, 0x709, 0);        /* "trigger required": ask the pin to sense first */
        bool now = (verb8(h, p->nid, 0xF09, 0) & 0x80000000u) != 0;
        if (now != p->plugged && !initial) klog("Nitori", "hda: pin 0x%02x (%s) %s", p->nid, hda_dev_name(p->dev), now ? "plugged in" : "unplugged");
        p->plugged = now;
    }
    hda_plan_automute(pl);
    for (int i = 0; i < pl->npins; i++) {
        bool want = pl->pin[i].want;
        if (initial || !h->pins_written || want != before[i]) pin_enable(h, &pl->pin[i], want);
    }
    h->pins_written = true;
}

/* ------------------------------------------------------------------ the output stream */

static void set_converters(Hda *h) {
    for (int i = 0; i < h->plan.ndacs; i++) {
        u32 dac = h->plan.dac[i];
        verb16(h, dac, 0x2, STREAM_FORMAT);
        verb8(h, dac, 0x706, STREAM_TAG << 4);          /* stream tag, channel 0 (left/right = channels 0 and 1) */
        u32 back = verb16(h, dac, 0xA, 0) & 0xFFFF;
        if (back != STREAM_FORMAT) klog("Nitori", "hda: converter 0x%02x kept format %04x instead of %04x", dac, back, STREAM_FORMAT);
    }
}

static void sd_stop(Hda *h) {
    u8 c = rd8(h, h->sd + SD_CTL);
    wr8(h, h->sd + SD_CTL, c & ~0x02u);
    wait_bits8(h, h->sd + SD_CTL, 0x02, 0, 20);
}

static int hda_start(AudioBackend *b) {
    Hda *h = b->priv;
    sd_stop(h);
    wr8(h, h->sd + SD_CTL, 0x01);                       /* stream reset: set, wait for 1, clear, wait for 0 */
    wait_bits8(h, h->sd + SD_CTL, 0x01, 0x01, 5);
    wr8(h, h->sd + SD_CTL, 0x00);
    wait_bits8(h, h->sd + SD_CTL, 0x01, 0x00, 5);
    wr8(h, h->sd + SD_STS, 0x1C);                       /* clear the status bits */
    wr8(h, h->sd + SD_CTL + 2, (u8)(STREAM_TAG << 4));
    wr32(h, h->sd + SD_CBL, RING_FRAMES * 4);
    wr16(h, h->sd + SD_LVI, BDL_ENTRIES - 1);
    wr16(h, h->sd + SD_FMT, STREAM_FORMAT);
    wr32(h, h->sd + SD_BDPL, (u32)h->bdl_phys);
    wr32(h, h->sd + SD_BDPU, h->ok64 ? (u32)(h->bdl_phys >> 32) : 0);
    if (h->posbuf) h->posbuf[(h->iss) * 2] = 0;
    h->last_lpib = 0;
    h->lpib_still = 0;
    set_converters(h);
    wr8(h, h->sd + SD_CTL, rd8(h, h->sd + SD_CTL) | 0x02);   /* RUN */
    if (!wait_bits8(h, h->sd + SD_CTL, 0x02, 0x02, 20)) return -EIO;
    return 0;
}

static void hda_stop(AudioBackend *b) { sd_stop(b->priv); }

/* LPIB is the position; if it sits at one value while the position buffer moves (a controller whose LPIB does not update),
 * the buffer takes over. The core notices a position that stands still by itself and falls back to a clock. */
static u32 hda_position(AudioBackend *b) {
    Hda *h = b->priv;
    u32 lpib = rd32(h, h->sd + SD_LPIB) % (RING_FRAMES * 4);
    u32 pb = h->posbuf ? h->posbuf[h->iss * 2] % (RING_FRAMES * 4) : 0;
    if (lpib != h->last_lpib) { h->last_lpib = lpib; h->lpib_still = 0; }
    else if (h->posbuf_on && pb != lpib && ++h->lpib_still > 200) h->use_posbuf = true;
    return (h->use_posbuf ? pb : lpib) / 4;
}

static void hda_housekeeping(AudioBackend *b) { apply_jacks(b->priv, false); }

static size_t hda_describe(AudioBackend *b, char *buf, size_t cap) {
    Hda *h = b->priv;
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "controller: %04x:%04x, %d input and %d output streams, %s, commands by %s, position from %s\n", h->pci->vendor,
                          h->pci->device, h->iss, h->oss, h->ok64 ? "64-bit DMA" : "32-bit DMA", h->use_imm ? "the immediate registers" : "CORB/RIRB",
                          h->use_posbuf ? "the position buffer" : "LPIB");
    n += (size_t)snprintf(buf + n, cap - n, "codec:    address %d, id %08x, subsystem %08x, function group 0x%02x, widgets 0x%02x..0x%02x\n", h->codec, h->vendor_id,
                          h->subsys_id, h->afg, h->graph.first, h->graph.first + h->graph.count - 1);
    HdaPlan *pl = &h->plan;
    for (int i = 0; i < pl->npins && n + 220 < cap; i++) {
        HdaOutPin *p = &pl->pin[i];
        const HdaWidget *w = hda_widget(&h->graph, p->nid);
        n += (size_t)snprintf(buf + n, cap - n, "output:   pin 0x%02x %s, config %08x, %s, %s; path", p->nid, hda_dev_name(p->dev), w ? w->pincfg : 0,
                              p->detect ? (p->plugged ? "jack plugged" : "jack empty") : "no jack sensing", p->want ? "on" : "off");
        for (u32 k = 0; k < p->path.n; k++) n += (size_t)snprintf(buf + n, cap - n, " 0x%02x", p->path.nid[k]);
        n += (size_t)snprintf(buf + n, cap - n, "\n");
    }
    if (pl->guessed && n + 80 < cap) n += (size_t)snprintf(buf + n, cap - n, "          (the BIOS declared no output pin; this one is a guess)\n");
    for (u32 i = 0; i < h->graph.count && n + 120 < cap; i++) {   /* the other pins, for diagnosing a jack that stays silent */
        const HdaWidget *w = &h->w[i];
        u32 nid = h->graph.first + i;
        if (w->type != HDA_WT_PIN) continue;
        bool chosen = false;
        for (int k = 0; k < pl->npins; k++) if (pl->pin[k].nid == nid) chosen = true;
        if (chosen) continue;
        n += (size_t)snprintf(buf + n, cap - n, "other pin 0x%02x: %s, config %08x, caps %08x%s\n", nid, hda_dev_name(HDA_CFG_DEV(w->pincfg)), w->pincfg,
                              w->pincap, HDA_CFG_CONN(w->pincfg) == 1 ? " (not connected)" : "");
    }
    return n;
}

/* ------------------------------------------------------------------ probing */

static bool hda_probe(PciDev *d) {
    Hda *h = &g_hda;
    memset(h, 0, sizeof *h);
    h->pci = d;
    pci_enable(d, PCI_CMD_MEM | PCI_CMD_MASTER);
    if (d->vendor == 0x8086) {                           /* Intel quirks Linux applies: traffic class 0, coherent (snooped) DMA */
        pci_write8(d, 0x44, pci_read8(d, 0x44) & ~0x07);
        u16 devc = pci_read16(d, 0x78);
        if (devc & 0x0800) pci_write16(d, 0x78, devc & ~0x0800);
    }
    h->mmio = pci_map_bar(d, 0);
    if (!h->mmio) { klog("Nitori", "hda %02x:%02x.%x: no register window", d->bus, d->dev, d->fn); return false; }
    u16 gcap = rd16(h, R_GCAP);
    h->ok64 = gcap & 1;
    h->iss = (gcap >> 8) & 0xF;
    h->oss = (gcap >> 12) & 0xF;
    if (h->oss == 0) { klog("Nitori", "hda %02x:%02x.%x: the controller has no output streams", d->bus, d->dev, d->fn); return false; }
    h->sd = R_SD0 + h->iss * 0x20;

    if (cmdline_flag("hda.imm")) { h->use_imm = true; h->tried_imm = true; }     /* debugging aids: exercise the fallbacks on a machine where they are not needed */
    u16 present = controller_up(h);
    if (!present) { klog("Nitori", "hda %02x:%02x.%x: no codec answered", d->bus, d->dev, d->fn); return false; }
    if (!cmd_rings_init(h)) { klog("Nitori", "hda: no memory for the command rings"); return false; }

    bool found = false;
    for (int a = 0; a < 15 && !found; a++) {
        if (!(present & (1u << a))) continue;
        h->dead = false;
        h->timeouts = 0;
        if (!parse_codec(h, a)) continue;
        hda_plan_outputs(&h->graph, &h->plan);
        if (h->plan.npins) found = true;
        else {
            klog("Nitori", "hda: codec %08x (address %d) has no usable analog output; its pins:", h->vendor_id, h->codec);
            for (u32 i = 0; i < h->graph.count && i < 48; i++) {
                const HdaWidget *w = &h->w[i];
                if (w->type == HDA_WT_PIN) klog("Nitori", "hda:   pin 0x%02x config %08x caps %08x conns %u", h->graph.first + i, w->pincfg, w->pincap, w->nconn);
            }
        }
    }
    if (h->dead) found = false;
    if (!found) { klog("Nitori", "hda %02x:%02x.%x: codecs found, but none with an analog output (HDMI only?)", d->bus, d->dev, d->fn); return false; }

    h->ring = dma_alloc(RING_FRAMES * 4, &h->ring_phys);
    h->bdl = dma_alloc(4096, &h->bdl_phys);
    h->posbuf = dma_alloc(4096, &h->posbuf_phys);
    if (!h->ring || !h->bdl) { klog("Nitori", "hda: no memory for the audio ring"); return false; }
    for (int i = 0; i < BDL_ENTRIES; i++) {
        u64 *e = (u64 *)(h->bdl + i * 16);
        e[0] = h->ring_phys + (u64)i * (RING_FRAMES * 4 / BDL_ENTRIES);
        ((u32 *)e)[2] = RING_FRAMES * 4 / BDL_ENTRIES;
        ((u32 *)e)[3] = 0;                               /* no interrupt on completion */
    }
    if (h->posbuf) {                                     /* ask the controller to keep stream positions in memory too */
        wr32(h, R_DPUBASE, h->ok64 ? (u32)(h->posbuf_phys >> 32) : 0);
        wr32(h, R_DPLBASE, (u32)h->posbuf_phys | 1);
        h->posbuf_on = (rd32(h, R_DPLBASE) & 1) != 0;
        if (h->posbuf_on && cmdline_flag("hda.posbuf")) h->use_posbuf = true;
    }

    if (cmdline_flag("hda.dump")) {                      /* the whole widget graph in the log, for diagnosing a codec on real hardware */
        klog("Nitori", "hda: codec %08x subsystem %08x, widgets 0x%02x..0x%02x", h->vendor_id, h->subsys_id, h->graph.first, h->graph.first + h->graph.count - 1);
        for (u32 i = 0; i < h->graph.count; i++) {
            const HdaWidget *w = &h->w[i];
            char conn[96];
            int n = 0;
            for (u32 k = 0; k < w->nconn && n < (int)sizeof conn - 6; k++) n += snprintf(conn + n, sizeof conn - (size_t)n, " %02x", w->conn[k]);
            conn[n] = 0;
            klog("Nitori", "hda:  0x%02x type %u caps %08x pincap %08x config %08x ampout %08x ampin %08x pcm %08x conn[%u]%s", h->graph.first + i, w->type,
                 w->caps, w->pincap, w->pincfg, w->ampout, w->ampin, w->pcm, w->nconn, conn);
        }
    }
    program_paths(h);
    apply_jacks(h, true);

    h->be.name = "Intel HD Audio";
    int len = snprintf(h->be.desc, sizeof h->be.desc, "codec %04x:%04x at address %d; output", h->vendor_id >> 16, h->vendor_id & 0xFFFF, h->codec);
    for (int i = 0; i < h->plan.npins && len < (int)sizeof h->be.desc - 40; i++)
        len += snprintf(h->be.desc + len, sizeof h->be.desc - (size_t)len, " %s(0x%02x<-0x%02x)", hda_dev_name(h->plan.pin[i].dev), h->plan.pin[i].nid,
                        h->plan.pin[i].path.nid[h->plan.pin[i].path.n - 1]);
    h->be.ring = (s16 *)h->ring;
    h->be.ring_frames = RING_FRAMES;
    h->be.start = hda_start;
    h->be.stop = hda_stop;
    h->be.position = hda_position;
    h->be.housekeeping = hda_housekeeping;
    h->be.describe = hda_describe;
    h->be.priv = h;
    d->driver = "hda";
    return audio_register(&h->be) == 0;
}

static void hda_init(void) {
    if (cmdline_flag("noaudio") || audio_registered()) return;
    for (PciDev *d = pci_find_class(0x04, 0x03, -1, NULL); d; d = pci_find_class(0x04, 0x03, -1, d)) {
        klog("Nitori", "hda %02x:%02x.%x %04x:%04x: looking for a codec", d->bus, d->dev, d->fn, d->vendor, d->device);
        if (hda_probe(d)) return;
    }
}
INITCALL(hda_init);
