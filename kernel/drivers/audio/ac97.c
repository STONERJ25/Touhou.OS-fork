/*
 * AC'97 (Intel ICH and compatibles): the small, old way to reach a codec. Polled, like everything else here.
 *
 * Two I/O windows: the mixer (the codec's registers, 16-bit) and the bus master (DMA engines, one buffer descriptor
 * list per stream). The PCM-out engine loops over a 256 KiB ring described by 32 descriptors of 8 KiB; the engine stops
 * when it reaches the "last valid index", so the poll keeps that index one behind the current one, which makes the list
 * circular. The play position is the current descriptor index plus how far into it the engine is.
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

/* mixer registers (offsets into the NAM window) */
#define NAM_RESET     0x00
#define NAM_MASTER    0x02
#define NAM_HEADPHONE 0x04
#define NAM_MONO      0x06
#define NAM_PCM_OUT   0x18
#define NAM_POWER     0x26
#define NAM_EXT_ID    0x28
#define NAM_EXT_CTRL  0x2A
#define NAM_VENDOR1   0x7C
#define NAM_VENDOR2   0x7E

/* bus master registers (offsets into the NABM window); the PCM-out engine starts at 0x10 */
#define PO_BDBAR  0x10
#define PO_CIV    0x14
#define PO_LVI    0x15
#define PO_SR     0x16
#define PO_PICB   0x18
#define PO_CR     0x1B
#define GLOB_CNT  0x2C
#define GLOB_STA  0x30
#define CAS       0x34

#define RING_FRAMES   65536
#define BD_COUNT      32
#define BD_SAMPLES    (RING_FRAMES * 2 / BD_COUNT)     /* 16-bit samples per descriptor */
#define BD_FRAMES     (RING_FRAMES / BD_COUNT)

typedef struct Ac97 {
    PciDev *pci;
    u16 nam, nabm;
    u8 *ring;   paddr_t ring_phys;
    u8 *bdl;    paddr_t bdl_phys;
    u32 vendor_id;
    bool running;
    AudioBackend be;
} Ac97;

static Ac97 g_ac97;

static inline u16 mix_rd(Ac97 *a, u32 reg) { return inw((u16)(a->nam + reg)); }
static inline void mix_wr(Ac97 *a, u32 reg, u16 v) { outw((u16)(a->nam + reg), v); }

/* The codec access semaphore: reading it returns 0 exactly once until it is released by writing 0. */
static void codec_lock(Ac97 *a) {
    for (int i = 0; i < 200; i++) {
        if (!(inb((u16)(a->nabm + CAS)) & 1)) return;
        udelay(50);
    }
}
static void codec_unlock(Ac97 *a) { outb((u16)(a->nabm + CAS), 0); }

static u8 civ_now(Ac97 *a) { return inb((u16)(a->nabm + PO_CIV)) & 0x1F; }

static int ac97_start(AudioBackend *b) {
    Ac97 *a = b->priv;
    outb((u16)(a->nabm + PO_CR), 0x00);
    outb((u16)(a->nabm + PO_CR), 0x02);                          /* reset the engine's registers */
    for (int i = 0; i < 2000 && (inb((u16)(a->nabm + PO_CR)) & 0x02); i++) udelay(50);
    outl((u16)(a->nabm + PO_BDBAR), (u32)a->bdl_phys);
    outb((u16)(a->nabm + PO_LVI), BD_COUNT - 1);
    outw((u16)(a->nabm + PO_SR), 0x1C);                          /* clear the status bits */
    outb((u16)(a->nabm + PO_CR), 0x01);                          /* run */
    a->running = true;
    return 0;
}

static void ac97_stop(AudioBackend *b) {
    Ac97 *a = b->priv;
    outb((u16)(a->nabm + PO_CR), 0x00);
    a->running = false;
}

static u32 ac97_position(AudioBackend *b) {
    Ac97 *a = b->priv;
    u32 civ = 0, picb = 0;
    for (int tries = 0; tries < 4; tries++) {                    /* CIV and PICB are read separately: retry if the engine moved to the next buffer */
        civ = civ_now(a);
        picb = inw((u16)(a->nabm + PO_PICB));
        if (civ_now(a) == civ) break;
    }
    /* keep the list circular: the last valid index trails the current one by one */
    outb((u16)(a->nabm + PO_LVI), (u8)((civ + BD_COUNT - 1) & 0x1F));
    if (inw((u16)(a->nabm + PO_SR)) & 0x01) {                    /* the engine halted (we were away for more than a whole ring) */
        outw((u16)(a->nabm + PO_SR), 0x1C);
        outb((u16)(a->nabm + PO_CR), 0x01);
    }
    if (picb > BD_SAMPLES) picb = BD_SAMPLES;
    return (civ * BD_FRAMES + (BD_SAMPLES - picb) / 2) % RING_FRAMES;
}

static size_t ac97_describe(AudioBackend *b, char *buf, size_t cap) {
    Ac97 *a = b->priv;
    return (size_t)snprintf(buf, cap, "ac97:     mixer 0x%x, bus master 0x%x, codec id %08x, master %04x, pcm %04x, status %04x\n", a->nam, a->nabm,
                            a->vendor_id, mix_rd(a, NAM_MASTER), mix_rd(a, NAM_PCM_OUT), (u32)inw((u16)(a->nabm + PO_SR)));
}

static bool ac97_probe(PciDev *d) {
    Ac97 *a = &g_ac97;
    memset(a, 0, sizeof *a);
    a->pci = d;
    if (!d->bar[0].io || !d->bar[1].io || d->bar[0].base > 0xFFFF || d->bar[1].base > 0xFFFF) {
        klog("Nitori", "ac97 %02x:%02x.%x: unexpected register windows", d->bus, d->dev, d->fn);
        return false;
    }
    a->nam = (u16)d->bar[0].base;
    a->nabm = (u16)d->bar[1].base;
    pci_enable(d, PCI_CMD_IO | PCI_CMD_MASTER);

    /* leave cold reset and wait for the primary codec */
    outl((u16)(a->nabm + GLOB_CNT), 0x00000002);
    bool ready = false;
    for (int i = 0; i < 1000; i++) {
        if (inl((u16)(a->nabm + GLOB_STA)) & 0x100) { ready = true; break; }
        task_sleep_ms(1);
    }
    if (!ready) { klog("Nitori", "ac97 %02x:%02x.%x: the codec never became ready", d->bus, d->dev, d->fn); return false; }

    codec_lock(a);
    mix_wr(a, NAM_RESET, 0xFFFF);
    codec_unlock(a);
    for (int i = 0; i < 500; i++) {                              /* the analog and converter sections report ready in the low bits */
        if ((mix_rd(a, NAM_POWER) & 0x0F) == 0x0F) break;
        task_sleep_ms(1);
    }
    a->vendor_id = ((u32)mix_rd(a, NAM_VENDOR1) << 16) | mix_rd(a, NAM_VENDOR2);

    /* unmuted at 0 dB; loudness is the mixer's business */
    codec_lock(a);
    mix_wr(a, NAM_MASTER, 0x0000);
    mix_wr(a, NAM_HEADPHONE, 0x0000);
    mix_wr(a, NAM_MONO, 0x0000);
    mix_wr(a, NAM_PCM_OUT, 0x0000);                          /* 0 dB: AC97 volumes are attenuations in 1.5 dB steps, 0 is full level */
    codec_unlock(a);

    a->ring = dma_alloc(RING_FRAMES * 4, &a->ring_phys);
    a->bdl = dma_alloc(4096, &a->bdl_phys);
    if (!a->ring || !a->bdl) { klog("Nitori", "ac97: no memory for the audio ring"); return false; }
    for (int i = 0; i < BD_COUNT; i++) {
        u32 *e = (u32 *)(a->bdl + i * 8);
        e[0] = (u32)(a->ring_phys + (u64)i * (BD_FRAMES * 4));
        e[1] = BD_SAMPLES;                                       /* length in samples, no interrupt flags */
    }

    a->be.name = "AC'97";
    snprintf(a->be.desc, sizeof a->be.desc, "codec %04x:%04x, front PCM out at 48 kHz", a->vendor_id >> 16, a->vendor_id & 0xFFFF);
    a->be.ring = (s16 *)a->ring;
    a->be.ring_frames = RING_FRAMES;
    a->be.start = ac97_start;
    a->be.stop = ac97_stop;
    a->be.position = ac97_position;
    a->be.describe = ac97_describe;
    a->be.priv = a;
    d->driver = "ac97";
    return audio_register(&a->be) == 0;
}

static void ac97_init(void) {
    if (cmdline_flag("noaudio") || audio_registered()) return;
    for (PciDev *d = pci_find_class(0x04, 0x01, -1, NULL); d; d = pci_find_class(0x04, 0x01, -1, d)) {
        if (d->driver) continue;                                 /* some other sound driver has it */
        klog("Nitori", "ac97 %02x:%02x.%x %04x:%04x: looking for a codec", d->bus, d->dev, d->fn, d->vendor, d->device);
        if (ac97_probe(d)) return;
    }
}
INITCALL(ac97_init);
