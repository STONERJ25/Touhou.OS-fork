/*
 * Intel High Definition Audio: controller, codec graph and the playback stream.
 *
 * Polled throughout (no interrupts, no MSI): commands go through the CORB/RIRB ring, the play position comes from the
 * stream descriptor's LPIB register. The driver
 *   1. resets the controller and waits for the codecs to announce themselves,
 *   2. reads every codec's audio function group: widget types and capabilities, connection lists, pin default
 *      configuration, pin capabilities and amplifier capabilities,
 *   3. finds, for every output pin that the BIOS declared as a line out, speaker or headphone jack, a path
 *      pin <- (mixers, selectors) <- DAC, powers the widgets, selects the connections, unmutes the amplifiers and
 *      switches on the pin and its EAPD,
 *   4. loops one output stream (48 kHz, 16-bit, stereo) over a 64 KiB ring for the audio core,
 *   5. watches the jacks: plugging in headphones silences the speaker (and the rear line out), like Linux's auto-mute.
 * Anything unexpected is logged once and the driver stays out of the way instead of making noise.
 */
#include "audiodev.h"
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
#define R_SD0       0x80                    /* stream descriptors, 0x20 bytes each: input streams first, then output */
#define SD_CTL      0x00                    /* 24 bits: byte 0 flags, byte 2 = stream tag << 4 */
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

/* widget types */
#define WT_OUT   0
#define WT_IN    1
#define WT_MIXER 2
#define WT_SEL   3
#define WT_PIN   4

#define MAX_CONN  32
#define MAX_PATH  8
#define MAX_PINS  8
#define STREAM_TAG 1
#define RING_FRAMES 16384
#define BDL_ENTRIES 16
#define STREAM_FORMAT 0x0011                /* 48 kHz, 16 bits, 2 channels */

typedef struct HdaWidget {
    u8  type;
    u32 caps;
    u8  nconn;
    u8  conn[MAX_CONN];
    u32 pincap, pincfg;
    u32 ampout, ampin;
    bool valid;
} HdaWidget;

typedef struct HdaPath {
    u8 n;                                   /* nid[0] is the pin, nid[n-1] the DAC */
    u8 nid[MAX_PATH];
    u8 idx[MAX_PATH];                       /* connection index of nid[k] that leads to nid[k+1] */
} HdaPath;

typedef struct HdaPin {
    u8  nid;
    u8  dev;                                /* default device: 0 line out, 1 speaker, 2 headphone */
    bool detect;                            /* presence detect capable */
    bool plugged;
    bool enabled;
    HdaPath path;
} HdaPin;

typedef struct Hda {
    PciDev *pci;
    volatile u8 *mmio;
    u32 *corb;  u64 *rirb;
    u32 corb_n, rirb_n, corb_wp, rirb_rp;
    int iss, oss;
    u8 *bdl;    paddr_t bdl_phys;
    u8 *ring;   paddr_t ring_phys;
    int codec;                              /* address of the codec in use */
    u8  afg, first, count;
    u32 vendor_id;
    HdaWidget *w;                           /* indexed by nid - first */
    HdaPin pins[MAX_PINS];
    int npins;
    int sd;                                 /* offset of our output stream descriptor */
    int timeouts;
    bool dead;                              /* the codec stopped answering: give up quietly */
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

static int cmd_raw(Hda *h, u32 cmd, u32 *resp) {
    if (h->dead) return -EIO;
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
            h->timeouts = 0;
            return 0;
        }
        udelay(25);
    }
    if (++h->timeouts >= 3) { h->dead = true; klog("Nitori", "hda: the codec stopped answering commands"); }
    return -ETIMEDOUT;
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
    udelay(1000);
    wr32(h, R_GCTL, rd32(h, R_GCTL) | 1u);              /* leave reset */
    if (!wait_bits32(h, R_GCTL, 1, 1, 200)) { klog("Nitori", "hda: the controller would not leave reset"); return false; }
    wr32(h, R_INTCTL, 0);                               /* polled: no interrupts */
    return true;
}

static bool cmd_rings_init(Hda *h) {
    paddr_t cp = 0, rp = 0;
    h->corb = dma_alloc(4096, &cp);
    h->rirb = dma_alloc(4096, &rp);
    if (!h->corb || !h->rirb) return false;

    u8 cs = rd8(h, R_CORBSIZE), rs = rd8(h, R_RIRBSIZE);
    h->corb_n = (cs & 0x40) ? 256 : (cs & 0x20) ? 16 : 2;
    h->rirb_n = (rs & 0x40) ? 256 : (rs & 0x20) ? 16 : 2;
    wr8(h, R_CORBSIZE, (cs & 0xFC) | (h->corb_n == 256 ? 2 : h->corb_n == 16 ? 1 : 0));
    wr8(h, R_RIRBSIZE, (rs & 0xFC) | (h->rirb_n == 256 ? 2 : h->rirb_n == 16 ? 1 : 0));

    wr8(h, R_CORBCTL, 0);                               /* DMA off while it is programmed */
    wr8(h, R_RIRBCTL, 0);
    wr32(h, R_CORBLBASE, (u32)cp);
    wr32(h, R_CORBUBASE, (u32)(cp >> 32));
    wr32(h, R_RIRBLBASE, (u32)rp);
    wr32(h, R_RIRBUBASE, (u32)(rp >> 32));

    wr16(h, R_CORBRP, 0x8000);                          /* reset the read pointer: set, wait for 1, clear, wait for 0 */
    for (int i = 0; i < 1000 && !(rd16(h, R_CORBRP) & 0x8000); i++) udelay(10);
    wr16(h, R_CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(h, R_CORBRP) & 0x8000); i++) udelay(10);
    wr16(h, R_CORBWP, 0);
    h->corb_wp = 0;

    wr16(h, R_RIRBWP, 0x8000);
    wr16(h, R_RINTCNT, 1);
    h->rirb_rp = 0;
    wr8(h, R_RIRBCTL, 0x02);                            /* response DMA on */
    wr8(h, R_CORBCTL, 0x02);                            /* command DMA on */
    return true;
}

/* ------------------------------------------------------------------ codec graph */

static HdaWidget *wid(Hda *h, u32 nid) {
    if (nid < h->first || nid >= (u32)h->first + h->count) return NULL;
    HdaWidget *w = &h->w[nid - h->first];
    return w->valid ? w : NULL;
}

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
            if (range && prev) {
                for (u32 x = prev + 1; x <= n && w->nconn < MAX_CONN; x++) w->conn[w->nconn++] = (u8)x;
            } else if (w->nconn < MAX_CONN) {
                w->conn[w->nconn++] = (u8)n;
            }
            prev = n;
        }
    }
}

static bool parse_codec(Hda *h, int addr) {
    h->codec = addr;
    u32 vid = param(h, 0, 0x00);
    if (!vid || vid == 0xFFFFFFFF) return false;
    u32 root = param(h, 0, 0x04);
    u32 fg0 = (root >> 16) & 0xFF, nfg = root & 0xFF;
    int afg = -1;
    for (u32 i = 0; i < nfg && i < 8; i++)
        if ((param(h, fg0 + i, 0x05) & 0xFF) == 1) { afg = (int)(fg0 + i); break; }
    if (afg < 0) return false;
    u32 sub = param(h, (u32)afg, 0x04);
    h->afg = (u8)afg;
    h->first = (u8)((sub >> 16) & 0xFF);
    h->count = (u8)(sub & 0xFF);
    h->vendor_id = vid;
    if (!h->count) return false;
    kfree(h->w);
    h->w = kzalloc(sizeof(HdaWidget) * h->count);
    if (!h->w) return false;

    verb8(h, (u32)afg, 0x705, 0);                       /* function group to D0 */
    task_sleep_ms(15);

    u32 afg_ampout = param(h, (u32)afg, 0x12), afg_ampin = param(h, (u32)afg, 0x0D);
    for (u32 i = 0; i < h->count; i++) {
        u32 nid = h->first + i;
        HdaWidget *w = &h->w[i];
        w->caps = param(h, nid, 0x09);
        w->type = (u8)((w->caps >> 20) & 0xF);
        w->valid = true;
        w->ampout = (w->caps & (1u << 3)) ? param(h, nid, 0x12) : afg_ampout;
        w->ampin = (w->caps & (1u << 3)) ? param(h, nid, 0x0D) : afg_ampin;
        if (w->caps & (1u << 8)) read_connections(h, nid, w);
        if (w->type == WT_PIN) {
            w->pincap = param(h, nid, 0x0C);
            w->pincfg = verb8(h, nid, 0xF1C, 0);
        }
    }
    return true;
}

static const char *const g_dev_names[] = {
    "line out", "speaker", "headphone", "CD", "S/PDIF out", "digital out", "modem line", "modem handset", "line in", "aux", "mic",
    "telephony", "S/PDIF in", "digital in", "reserved", "other",
};

/* Depth-first search from a pin back to a DAC through mixers and selectors; avoid[] holds DACs already given to other pins. */
static bool find_dac(Hda *h, u32 nid, int depth, HdaPath *p, u8 *visited, const u8 *avoid) {
    HdaWidget *w = wid(h, nid);
    if (!w) return false;
    p->nid[depth] = (u8)nid;
    if (w->type == WT_OUT) {
        if (avoid && avoid[nid]) return false;
        p->n = (u8)(depth + 1);
        return true;
    }
    if (depth >= MAX_PATH - 1) return false;
    if (!(w->type == WT_MIXER || w->type == WT_SEL || (w->type == WT_PIN && depth == 0))) return false;
    visited[nid] = 1;
    for (u32 i = 0; i < w->nconn; i++) {
        u32 c = w->conn[i];
        if (visited[c]) continue;
        p->idx[depth] = (u8)i;
        if (find_dac(h, c, depth + 1, p, visited, avoid)) return true;
    }
    visited[nid] = 0;
    return false;
}

static void collect_pins(Hda *h) {
    u8 used[256];
    memset(used, 0, sizeof used);
    h->npins = 0;
    for (int pass = 0; pass < 2; pass++) {               /* pass 0: the BIOS-declared outputs; pass 1: guess if there were none */
        for (u32 i = 0; i < h->count && h->npins < MAX_PINS; i++) {
            HdaWidget *w = &h->w[i];
            u32 nid = h->first + i;
            if (w->type != WT_PIN || !(w->pincap & (1u << 4))) continue;          /* not an output-capable pin */
            u32 conn = w->pincfg >> 30, dev = (w->pincfg >> 20) & 0xF;
            bool declared = conn != 1 && dev <= 2;
            if (pass == 0 && !declared) continue;
            if (pass == 1 && (h->npins || conn == 1)) continue;
            bool dup = false;
            for (int k = 0; k < h->npins; k++) if (h->pins[k].nid == nid) dup = true;
            if (dup) continue;
            HdaPath path;
            u8 visited[256];
            memset(visited, 0, sizeof visited);
            memset(&path, 0, sizeof path);
            bool ok = find_dac(h, nid, 0, &path, visited, used);
            if (!ok) {
                memset(visited, 0, sizeof visited);
                ok = find_dac(h, nid, 0, &path, visited, NULL);
            }
            if (!ok) continue;
            HdaPin *p = &h->pins[h->npins++];
            p->nid = (u8)nid;
            p->dev = (u8)(dev <= 2 ? dev : 0);
            p->detect = (w->pincap & (1u << 2)) && conn != 2;
            p->path = path;
            used[path.nid[path.n - 1]] = 1;
        }
        if (h->npins) break;
    }
}

/* ------------------------------------------------------------------ programming the path */

static u32 amp_zero_db(u32 cap) { return cap & 0x7F; }  /* the offset field is the step that means 0 dB */

static void set_amp_out(Hda *h, u32 nid, u32 cap, bool unmute) {
    if (!(cap & 0x7FFFFFFF) && !(cap >> 31)) return;
    u32 val = unmute ? amp_zero_db(cap) : ((cap >> 31) ? 0x80 : 0);
    verb16(h, nid, 0x3, 0xB000 | val);                  /* output amp, left and right */
}

static void set_amp_in(Hda *h, u32 nid, u32 cap, u32 index, bool unmute) {
    if (!(cap & 0x7FFFFFFF) && !(cap >> 31)) return;
    u32 val = unmute ? amp_zero_db(cap) : ((cap >> 31) ? 0x80 : 0);
    verb16(h, nid, 0x3, 0x7000 | (index << 8) | val);   /* input amp, left and right, this connection */
}

static void power_up(Hda *h, u32 nid, HdaWidget *w) {
    if (!(w->caps & (1u << 10))) return;                /* no power control: always on */
    verb8(h, nid, 0x705, 0);
    for (int i = 0; i < 100; i++) {
        u32 r = verb8(h, nid, 0xF05, 0);
        if (((r >> 4) & 3) == 0) break;
        task_sleep_ms(1);
    }
}

/* Selects the connections, powers the widgets and unmutes everything on the path (the pin itself is switched separately). */
static void program_path(Hda *h, HdaPin *p) {
    HdaPath *pa = &p->path;
    for (u32 k = 0; k < pa->n; k++) {
        u32 nid = pa->nid[k];
        HdaWidget *w = wid(h, nid);
        if (!w) continue;
        power_up(h, nid, w);
        if (k + 1 < pa->n) {
            if (w->nconn > 1 && (w->type == WT_SEL || w->type == WT_PIN)) verb8(h, nid, 0x701, pa->idx[k]);
            if (w->caps & (1u << 1)) {                  /* input amps: only the connection we use is open */
                if (w->type == WT_MIXER) {
                    for (u32 i = 0; i < w->nconn; i++) set_amp_in(h, nid, w->ampin, i, i == pa->idx[k]);
                } else {
                    set_amp_in(h, nid, w->ampin, pa->idx[k], true);
                }
            }
        }
        if (w->caps & (1u << 2)) set_amp_out(h, nid, w->ampout, true);
    }
}

static void pin_enable(Hda *h, HdaPin *p, bool on) {
    HdaWidget *w = wid(h, p->nid);
    if (!w) return;
    if (on) {
        u32 ctl = 0x40;                                 /* OUT_EN */
        if (p->dev == 2 && (w->pincap & (1u << 3))) ctl |= 0x80;     /* HP_EN on headphone drive capable pins */
        verb8(h, p->nid, 0x707, ctl);
        if (w->pincap & (1u << 16)) verb8(h, p->nid, 0x70C, 0x02);   /* EAPD on */
        if (w->caps & (1u << 2)) set_amp_out(h, p->nid, w->ampout, true);
    } else {
        verb8(h, p->nid, 0x707, 0);
        if (w->pincap & (1u << 16)) verb8(h, p->nid, 0x70C, 0x00);
    }
    p->enabled = on;
}

/* Headphones silence the speaker and the line out; a plugged line out silences the speaker. Jacks that cannot be sensed
 * count as always plugged. */
static void apply_jacks(Hda *h, bool initial) {
    bool hp = false, lo = false;
    for (int i = 0; i < h->npins; i++) {
        HdaPin *p = &h->pins[i];
        if (!p->detect) { p->plugged = true; continue; }
        bool now = (verb8(h, p->nid, 0xF09, 0) & 0x80000000u) != 0;
        if (now != p->plugged && !initial)
            klog("Nitori", "hda: pin 0x%02x (%s) %s", p->nid, g_dev_names[p->dev], now ? "plugged in" : "unplugged");
        p->plugged = now;
        if (p->dev == 2 && now) hp = true;
        if (p->dev == 0 && now) lo = true;
    }
    bool have_detect = false;
    for (int i = 0; i < h->npins; i++) if (h->pins[i].detect) have_detect = true;
    for (int i = 0; i < h->npins; i++) {
        HdaPin *p = &h->pins[i];
        bool want;
        if (!have_detect) want = true;
        else if (p->dev == 2) want = p->detect ? p->plugged : true;
        else if (p->dev == 0) want = (p->detect ? p->plugged : true) && !hp;
        else want = !hp && !lo;
        if (want != p->enabled || initial) pin_enable(h, p, want);
    }
}

/* ------------------------------------------------------------------ the output stream */

static void set_converters(Hda *h) {
    for (int i = 0; i < h->npins; i++) {
        u32 dac = h->pins[i].path.nid[h->pins[i].path.n - 1];
        verb16(h, dac, 0x2, STREAM_FORMAT);
        verb8(h, dac, 0x706, STREAM_TAG << 4);          /* stream tag, channel 0 (left/right = channels 0 and 1) */
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
    wr32(h, h->sd + SD_BDPU, (u32)(h->bdl_phys >> 32));
    set_converters(h);
    wr8(h, h->sd + SD_CTL, rd8(h, h->sd + SD_CTL) | 0x02);   /* RUN */
    if (!wait_bits8(h, h->sd + SD_CTL, 0x02, 0x02, 20)) return -EIO;
    return 0;
}

static void hda_stop(AudioBackend *b) { sd_stop(b->priv); }

static u32 hda_position(AudioBackend *b) {
    Hda *h = b->priv;
    return (rd32(h, h->sd + SD_LPIB) % (RING_FRAMES * 4)) / 4;
}

static void hda_housekeeping(AudioBackend *b) { apply_jacks(b->priv, false); }

static size_t hda_describe(AudioBackend *b, char *buf, size_t cap) {
    Hda *h = b->priv;
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "codec:    address %d, vendor/device %08x, function group 0x%02x, widgets 0x%02x..0x%02x\n", h->codec, h->vendor_id,
                          h->afg, h->first, h->first + h->count - 1);
    for (int i = 0; i < h->npins && n + 200 < cap; i++) {
        HdaPin *p = &h->pins[i];
        HdaWidget *w = wid(h, p->nid);
        n += (size_t)snprintf(buf + n, cap - n, "pin 0x%02x:  %s, config %08x, %s, %s; path", p->nid, g_dev_names[p->dev], w ? w->pincfg : 0,
                              p->detect ? (p->plugged ? "jack plugged" : "jack empty") : "no jack sensing", p->enabled ? "on" : "off");
        for (u32 k = 0; k < p->path.n; k++) n += (size_t)snprintf(buf + n, cap - n, " 0x%02x", p->path.nid[k]);
        n += (size_t)snprintf(buf + n, cap - n, "\n");
    }
    return n;
}

/* ------------------------------------------------------------------ probing */

static bool codec_has_analog_out(Hda *h) {
    collect_pins(h);
    return h->npins > 0;
}

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
    h->iss = (gcap >> 8) & 0xF;
    h->oss = (gcap >> 12) & 0xF;
    if (h->oss == 0) { klog("Nitori", "hda %02x:%02x.%x: the controller has no output streams", d->bus, d->dev, d->fn); return false; }
    h->sd = R_SD0 + h->iss * 0x20;

    if (!controller_reset(h)) return false;
    u16 present = 0;
    for (int i = 0; i < 500 && !present; i++) { present = rd16(h, R_STATESTS) & 0x7FFF; if (!present) task_sleep_ms(1); }
    if (!present) { klog("Nitori", "hda %02x:%02x.%x: no codec answered", d->bus, d->dev, d->fn); return false; }
    wr16(h, R_STATESTS, present);
    if (!cmd_rings_init(h)) { klog("Nitori", "hda: no memory for the command rings"); return false; }

    bool found = false;
    for (int a = 0; a < 15 && !found; a++) {
        if (!(present & (1u << a))) continue;
        h->dead = false;
        h->timeouts = 0;
        if (!parse_codec(h, a)) continue;
        if (codec_has_analog_out(h)) found = true;
    }
    if (h->dead) found = false;
    if (!found) { klog("Nitori", "hda %02x:%02x.%x: codecs found, but none with an analog output (HDMI only?)", d->bus, d->dev, d->fn); return false; }

    h->ring = dma_alloc(RING_FRAMES * 4, &h->ring_phys);
    h->bdl = dma_alloc(4096, &h->bdl_phys);
    if (!h->ring || !h->bdl) { klog("Nitori", "hda: no memory for the audio ring"); return false; }
    for (int i = 0; i < BDL_ENTRIES; i++) {
        u64 *e = (u64 *)(h->bdl + i * 16);
        e[0] = h->ring_phys + (u64)i * (RING_FRAMES * 4 / BDL_ENTRIES);
        ((u32 *)e)[2] = RING_FRAMES * 4 / BDL_ENTRIES;
        ((u32 *)e)[3] = 0;                               /* no interrupt on completion */
    }

    for (int i = 0; i < h->npins; i++) program_path(h, &h->pins[i]);
    apply_jacks(h, true);

    h->be.name = "Intel HD Audio";
    int len = snprintf(h->be.desc, sizeof h->be.desc, "codec %04x:%04x at address %d; output", h->vendor_id >> 16, h->vendor_id & 0xFFFF, h->codec);
    for (int i = 0; i < h->npins && len < (int)sizeof h->be.desc - 40; i++)
        len += snprintf(h->be.desc + len, sizeof h->be.desc - (size_t)len, " %s(0x%02x<-0x%02x)", g_dev_names[h->pins[i].dev], h->pins[i].nid,
                        h->pins[i].path.nid[h->pins[i].path.n - 1]);
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
    if (cmdline_flag("noaudio")) return;
    for (PciDev *d = pci_find_class(0x04, 0x03, -1, NULL); d; d = pci_find_class(0x04, 0x03, -1, d)) {
        klog("Nitori", "hda %02x:%02x.%x %04x:%04x: looking for a codec", d->bus, d->dev, d->fn, d->vendor, d->device);
        if (hda_probe(d)) return;
    }
}
INITCALL(hda_init);
