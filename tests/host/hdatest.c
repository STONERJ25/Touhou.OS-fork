/*
 * Host-side test of the HDA output planner (kernel/drivers/audio/hda_graph.c): which pins are driven, through which
 * mixers and selectors, from which converters, and which pins the auto-mute rules switch on.
 *
 * The codecs below are INVENTED. They imitate how Realtek ALC662 / ALC255 style codecs are built (several converters, mixers
 * that add converters and input pins, selectors, bidirectional pins, a digital converter, pin default configurations like
 * the ones the BIOS of a desktop writes) but none of them was recorded from real hardware; the real Dell's codec may differ.
 * What the test proves is that the planner copes with those shapes, not that it is right for one particular chip.
 *
 *   clang -Wall -Wextra tests/host/hdatest.c -o hdatest && ./hdatest        (tests/case_audio.py audio_hda_graph does this)
 */
#include "../../kernel/drivers/audio/hda_graph.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ building codecs */

typedef struct Fx { HdaWidget w[256]; HdaGraph g; const char *name; } Fx;

#define QEMU_PCM   ((1u << 17) | 0x1FC)          /* 16-bit, 8 kHz .. 96 kHz */
#define ALC_PCM    ((1u << 17) | (1u << 18) | (1u << 19) | 0x560)    /* 16/20/24 bit; 44.1, 48, 96 kHz ... */
#define AMP_STEREO(mute, steps, offset) (((mute) ? 0x80000000u : 0) | (3u << 16) | ((unsigned)(steps) << 8) | (unsigned)(offset))

#define PIN_OUT_ONLY (HDA_PC_OUT | HDA_PC_PRESENCE | HDA_PC_HP_DRIVE | HDA_PC_EAPD)
#define PIN_BIDIR    (HDA_PC_IN | HDA_PC_OUT | HDA_PC_PRESENCE | HDA_PC_HP_DRIVE | 3u)
#define PIN_IN_ONLY  (HDA_PC_IN | HDA_PC_PRESENCE)
#define PIN_FIXED    (HDA_PC_OUT | HDA_PC_EAPD)
#define PIN_DIGITAL  (HDA_PC_OUT | HDA_PC_PRESENCE)

static void begin(Fx *f, const char *name, int first, int last) {
    memset(f, 0, sizeof *f);
    f->name = name;
    f->g.first = (uint8_t)first;
    f->g.count = (uint8_t)(last - first + 1);
    f->g.w = f->w;
}

static HdaWidget *slot(Fx *f, int nid) {
    HdaWidget *w = &f->w[nid - f->g.first];
    w->valid = true;
    return w;
}

static void conns(HdaWidget *w, int n, va_list ap) {
    w->nconn = (uint8_t)n;
    for (int i = 0; i < n; i++) w->conn[i] = (uint8_t)va_arg(ap, int);
    if (n) w->caps |= HDA_WC_CONN_LIST;
}

static void dac(Fx *f, int nid, bool digital, uint32_t pcm) {
    HdaWidget *w = slot(f, nid);
    w->type = HDA_WT_OUT;
    w->caps = (HDA_WT_OUT << 20) | (1u << 16) | HDA_WC_STEREO | HDA_WC_AMP_OUT | HDA_WC_AMP_OVRD | HDA_WC_FMT_OVRD | HDA_WC_POWER | (digital ? HDA_WC_DIGITAL : 0);
    w->ampout = AMP_STEREO(true, 0x57, 0x57);
    w->pcm = pcm;
}

static void adc(Fx *f, int nid, int n, ...) {
    HdaWidget *w = slot(f, nid);
    w->type = HDA_WT_IN;
    w->caps = (HDA_WT_IN << 20) | HDA_WC_STEREO | HDA_WC_AMP_IN | HDA_WC_POWER;
    va_list ap;
    va_start(ap, n);
    conns(w, n, ap);
    va_end(ap);
}

static void mixer(Fx *f, int nid, int n, ...) {
    HdaWidget *w = slot(f, nid);
    w->type = HDA_WT_MIXER;
    w->caps = (HDA_WT_MIXER << 20) | HDA_WC_STEREO | HDA_WC_AMP_IN | HDA_WC_POWER;
    w->ampin = AMP_STEREO(true, 0x1F, 0x17);
    va_list ap;
    va_start(ap, n);
    conns(w, n, ap);
    va_end(ap);
}

static void selector(Fx *f, int nid, int n, ...) {
    HdaWidget *w = slot(f, nid);
    w->type = HDA_WT_SEL;
    w->caps = (HDA_WT_SEL << 20) | HDA_WC_STEREO | HDA_WC_POWER;
    va_list ap;
    va_start(ap, n);
    conns(w, n, ap);
    va_end(ap);
}

static void pin(Fx *f, int nid, uint32_t pincap, uint32_t pincfg, int n, ...) {
    HdaWidget *w = slot(f, nid);
    w->type = HDA_WT_PIN;
    w->caps = (HDA_WT_PIN << 20) | HDA_WC_STEREO | HDA_WC_AMP_OUT | HDA_WC_POWER;
    w->ampout = AMP_STEREO(true, 0x27, 0x27);
    w->pincap = pincap;
    w->pincfg = pincfg;
    va_list ap;
    va_start(ap, n);
    conns(w, n, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ checking plans */

static const HdaOutPin *pin_of(const HdaPlan *p, int nid) {
    for (int i = 0; i < p->npins; i++) if (p->pin[i].nid == nid) return &p->pin[i];
    return NULL;
}

static void describe(const Fx *f, const HdaPlan *p) {
    printf("%s:", f->name);
    for (int i = 0; i < p->npins; i++) {
        const HdaOutPin *o = &p->pin[i];
        printf("  %s 0x%02x", hda_dev_name(o->dev), o->nid);
        for (unsigned k = 1; k < o->path.n; k++) printf(" <- 0x%02x", o->path.nid[k]);
    }
    printf("%s  [%d converter%s]\n", p->guessed ? " (guessed)" : "", p->ndacs, p->ndacs == 1 ? "" : "s");
}

/* Expects the pin to exist with exactly this path: nid[0] is the pin, then the widgets down to the converter. */
static void expect_path(const HdaPlan *p, int n, const int *want, const char *fixture) {
    const HdaOutPin *o = pin_of(p, want[0]);
    CHECK(o != NULL, "%s: pin 0x%02x was not chosen", fixture, want[0]);
    if (!o) return;
    bool same = o->path.n == n;
    for (int i = 0; same && i < n; i++) same = o->path.nid[i] == want[i];
    if (!same) {
        printf("FAIL line %d: %s: pin 0x%02x path is", __LINE__, fixture, want[0]);
        for (unsigned i = 0; i < o->path.n; i++) printf(" 0x%02x", o->path.nid[i]);
        printf(", expected");
        for (int i = 0; i < n; i++) printf(" 0x%02x", want[i]);
        printf("\n");
        failures++;
    }
    checks++;
}

#define PATH(plan, fx, ...) do { const int _w[] = { __VA_ARGS__ }; expect_path(plan, (int)(sizeof _w / sizeof _w[0]), _w, fx); } while (0)

static void set_plugged(HdaPlan *p, int nid, bool v) {
    for (int i = 0; i < p->npins; i++) if (p->pin[i].nid == nid) p->pin[i].plugged = v;
}
static bool wants(const HdaPlan *p, int nid) {
    const HdaOutPin *o = pin_of(p, nid);
    return o && o->want;
}

/* ------------------------------------------------------------------ the codecs */

/* QEMU's hda-output: one converter, one line out pin, nothing else. */
static void qemu_output(Fx *f) {
    begin(f, "qemu hda-output", 2, 3);
    dac(f, 2, false, QEMU_PCM);
    pin(f, 3, HDA_PC_OUT, 0x00004010, 1, 2);
}

/* QEMU's hda-duplex: the same plus a line in pin and the converter that records it. */
static void qemu_duplex(Fx *f) {
    begin(f, "qemu hda-duplex", 2, 5);
    dac(f, 2, false, QEMU_PCM);
    adc(f, 3, 1, 5);
    pin(f, 4, HDA_PC_OUT, 0x00004010, 1, 2);
    pin(f, 5, HDA_PC_IN, 0x00814020, 0);
}

/* A desktop board with an ALC662-like codec: three analog converters and a digital one, one mixer per converter that also
 * takes the input pins, a 5.1 set of rear jacks (one association), a front headphone jack that can use a mixer or a
 * converter directly, microphone and line in jacks that are bidirectional pins, and S/PDIF out. */
static void alc662_like(Fx *f, bool front_hp_declared) {
    begin(f, front_hp_declared ? "alc662-like desktop" : "alc662-like, front jack unused", 2, 0x1e);
    dac(f, 2, false, ALC_PCM);  dac(f, 3, false, ALC_PCM);  dac(f, 4, false, ALC_PCM);
    dac(f, 6, true, ALC_PCM);
    adc(f, 8, 1, 0x0b);  adc(f, 9, 1, 0x0b);
    mixer(f, 0x0b, 4, 0x18, 0x19, 0x1a, 0x1b);
    mixer(f, 0x0c, 4, 0x02, 0x18, 0x19, 0x1a);
    mixer(f, 0x0d, 4, 0x03, 0x18, 0x19, 0x1a);
    mixer(f, 0x0e, 4, 0x04, 0x18, 0x19, 0x1a);
    pin(f, 0x14, PIN_OUT_ONLY, 0x01014010, 1, 0x0c);            /* rear green: front L/R of the 5.1 group */
    pin(f, 0x15, PIN_OUT_ONLY, 0x01011011, 1, 0x0d);            /* rear black: surround */
    pin(f, 0x16, PIN_OUT_ONLY, 0x01016012, 1, 0x0e);            /* rear orange: centre and bass */
    pin(f, 0x18, PIN_BIDIR, 0x01a19030, 2, 0x0c, 0x0d);         /* rear microphone */
    pin(f, 0x19, PIN_BIDIR, 0x02a19040, 2, 0x0c, 0x0d);         /* front microphone */
    pin(f, 0x1a, PIN_BIDIR, 0x01813050, 3, 0x0c, 0x0d, 0x0e);   /* rear line in */
    pin(f, 0x1b, PIN_BIDIR, front_hp_declared ? 0x02214020 : 0x411111f0, 2, 0x0d, 0x03);   /* front headphone: a mixer or DAC 3 */
    pin(f, 0x1e, PIN_DIGITAL, 0x01451130, 1, 0x06);             /* S/PDIF out */
}

/* A desktop (Dell OptiPlex style) with an ALC255/ALC3234-like codec: two analog converters, an output mixer, a selector in
 * front of the headphone pin, the rear line out and front headphone jacks, an unused speaker pin. */
static void alc255_like(Fx *f, bool laptop) {
    begin(f, laptop ? "alc255-like, internal speaker" : "alc255-like desktop", 2, 0x21);
    dac(f, 2, false, ALC_PCM);  dac(f, 3, false, ALC_PCM);
    dac(f, 6, true, ALC_PCM);
    adc(f, 8, 1, 0x0b);  adc(f, 9, 1, 0x0b);
    mixer(f, 0x0b, 4, 0x18, 0x19, 0x1a, 0x1b);
    mixer(f, 0x0f, 4, 0x02, 0x03, 0x18, 0x1a);
    selector(f, 0x0d, 2, 0x02, 0x03);
    pin(f, 0x12, PIN_IN_ONLY, 0x40000000, 0);                   /* digital microphone, not connected */
    pin(f, 0x14, laptop ? PIN_FIXED : PIN_OUT_ONLY, laptop ? 0x90170110 : 0x411111f0, 1, 0x0f);   /* speaker */
    pin(f, 0x18, PIN_BIDIR, 0x01a1903e, 1, 0x0f);               /* rear microphone */
    pin(f, 0x19, PIN_BIDIR, 0x02a1913f, 1, 0x0f);               /* front headset microphone */
    pin(f, 0x1a, PIN_BIDIR, 0x01813140, 1, 0x0f);               /* rear line in */
    pin(f, 0x1b, PIN_BIDIR, laptop ? 0x411111f0 : 0x01014010, 1, 0x0f);   /* rear line out */
    pin(f, 0x1e, PIN_DIGITAL, 0x01451130, 1, 0x06);             /* S/PDIF out */
    pin(f, 0x21, PIN_OUT_ONLY, 0x0221401f, 1, 0x0d);            /* front headphone jack, behind the selector */
}

/* ------------------------------------------------------------------ cases */

static void test_qemu(void) {
    Fx f;  HdaPlan p;
    qemu_output(&f);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 1 && p.ndacs == 1 && p.dac[0] == 2, "qemu hda-output: %d pins, %d converters", p.npins, p.ndacs);
    PATH(&p, f.name, 3, 2);
    CHECK(!p.pin[0].detect, "a pin without presence detect must not be treated as sensing");
    hda_plan_automute(&p);
    CHECK(wants(&p, 3), "the only output must be on");

    qemu_duplex(&f);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 1 && !pin_of(&p, 5), "the line in pin must not be an output");
    PATH(&p, f.name, 4, 2);
}

static void test_alc662(void) {
    Fx f;  HdaPlan p;
    alc662_like(&f, true);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 2, "alc662-like: expected the green jack and the front headphones, got %d pins", p.npins);
    CHECK(!pin_of(&p, 0x15) && !pin_of(&p, 0x16), "the surround and centre/bass jacks repeat the first jack of their group");
    CHECK(!pin_of(&p, 0x1e), "S/PDIF is not the analog output");
    CHECK(!pin_of(&p, 0x18) && !pin_of(&p, 0x19) && !pin_of(&p, 0x1a), "input jacks must not be outputs");
    PATH(&p, f.name, 0x14, 0x0c, 0x02);
    PATH(&p, f.name, 0x1b, 0x0d, 0x03);
    CHECK(p.ndacs == 2 && p.dac[0] == 2 && p.dac[1] == 3, "converters %d: 0x%02x 0x%02x", p.ndacs, p.dac[0], p.dac[1]);
    CHECK(pin_of(&p, 0x14) && pin_of(&p, 0x14)->detect, "the rear jack senses plugging");

    /* auto-mute: nothing plugged (or sensing broken) leaves everything on; headphones take the line out away */
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x14) && wants(&p, 0x1b), "nothing plugged: everything stays on rather than silence");
    set_plugged(&p, 0x1b, true);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x1b) && !wants(&p, 0x14), "headphones plugged: only the headphones");
    set_plugged(&p, 0x14, true);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x1b) && !wants(&p, 0x14), "both plugged: the headphones win");
    set_plugged(&p, 0x1b, false);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x14) && !wants(&p, 0x1b), "line out plugged only: the line out");

    alc662_like(&f, false);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 1, "front jack marked unused by the BIOS: %d pins", p.npins);
    PATH(&p, f.name, 0x14, 0x0c, 0x02);
}

static void test_alc255(void) {
    Fx f;  HdaPlan p;
    alc255_like(&f, false);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 2, "alc255-like desktop: %d pins", p.npins);
    PATH(&p, f.name, 0x1b, 0x0f, 0x02);
    PATH(&p, f.name, 0x21, 0x0d, 0x03);
    const HdaOutPin *hp = pin_of(&p, 0x21);
    CHECK(hp && hp->path.idx[1] == 1, "the selector must be told to pick its second input (the second converter)");
    CHECK(!pin_of(&p, 0x14), "the speaker pin is marked unused");
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x1b) && wants(&p, 0x21), "nothing plugged: both on");
    set_plugged(&p, 0x21, true);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x21) && !wants(&p, 0x1b), "headphones plugged: the rear line out is silenced");

    alc255_like(&f, true);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 2 && !pin_of(&p, 0x1b), "laptop-like: speaker and headphones, no line out");
    PATH(&p, f.name, 0x14, 0x0f, 0x02);
    PATH(&p, f.name, 0x21, 0x0d, 0x03);
    CHECK(pin_of(&p, 0x14) && !pin_of(&p, 0x14)->detect, "an internal speaker has no jack to sense");
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x14) && !wants(&p, 0x21), "laptop: the speaker plays, the empty headphone jack is off");
    set_plugged(&p, 0x21, true);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x21) && !wants(&p, 0x14), "laptop: headphones plugged in, the speaker is silenced");
    set_plugged(&p, 0x21, false);
    hda_plan_automute(&p);
    CHECK(wants(&p, 0x14) && !wants(&p, 0x21), "laptop: headphones unplugged, the speaker comes back");
}

static void test_shared_selector(void) {
    Fx f;  HdaPlan p;
    begin(&f, "two pins behind one selector", 2, 0x20);
    dac(&f, 2, false, ALC_PCM);  dac(&f, 3, false, ALC_PCM);
    selector(&f, 0x20, 2, 2, 3);
    pin(&f, 0x10, PIN_OUT_ONLY, 0x01014010, 1, 0x20);
    pin(&f, 0x11, PIN_OUT_ONLY, 0x02214020, 1, 0x20);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 2 && p.ndacs == 1, "a selector can pick one input: both pins must share converter 2 (%d pins, %d converters)", p.npins, p.ndacs);
    PATH(&p, f.name, 0x10, 0x20, 0x02);
    PATH(&p, f.name, 0x11, 0x20, 0x02);
}

static void test_odd_graphs(void) {
    Fx f;  HdaPlan p;

    begin(&f, "hdmi only", 2, 5);
    dac(&f, 2, true, ALC_PCM);  dac(&f, 3, true, ALC_PCM);
    pin(&f, 4, PIN_DIGITAL, 0x18560010, 1, 2);
    pin(&f, 5, PIN_DIGITAL, 0x18560020, 1, 3);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 0, "an HDMI-only codec has no analog output (%d pins)", p.npins);

    begin(&f, "bios declared nothing", 2, 5);
    dac(&f, 2, false, ALC_PCM);
    pin(&f, 3, PIN_OUT_ONLY, 0x01f00000, 1, 2);                 /* "other" device with a jack */
    pin(&f, 4, PIN_OUT_ONLY, 0x411111f0, 1, 2);                 /* not connected */
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 1 && p.guessed && pin_of(&p, 3), "one guessed pin expected (%d pins, guessed %d)", p.npins, p.guessed);

    begin(&f, "everything unused", 2, 4);
    dac(&f, 2, false, ALC_PCM);
    pin(&f, 3, PIN_OUT_ONLY, 0x411111f0, 1, 2);
    pin(&f, 4, PIN_OUT_ONLY, 0x411111f0, 1, 2);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 0, "pins marked 'no connection' must stay silent");

    begin(&f, "converter without 48 kHz", 2, 5);
    dac(&f, 2, false, (1u << 17) | (1u << 5));                  /* 44.1 kHz only */
    dac(&f, 3, false, ALC_PCM);
    pin(&f, 4, PIN_OUT_ONLY, 0x01014010, 2, 2, 3);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    CHECK(p.npins == 1, "the pin must find the second converter");
    PATH(&p, f.name, 4, 3);

    begin(&f, "no usable converter", 2, 4);
    dac(&f, 2, false, (1u << 16) | (1u << 6));                  /* 8-bit only */
    pin(&f, 3, PIN_OUT_ONLY, 0x01014010, 1, 2);
    hda_plan_outputs(&f.g, &p);
    CHECK(p.npins == 0, "a converter that cannot do 16-bit 48 kHz is no use");

    begin(&f, "unknown converter capabilities", 2, 3);
    dac(&f, 2, false, 0);
    pin(&f, 3, PIN_OUT_ONLY, 0x01014010, 1, 2);
    hda_plan_outputs(&f.g, &p);
    CHECK(p.npins == 1, "an unreadable capability word gets the benefit of the doubt");

    begin(&f, "mixers in a loop", 2, 0x12);
    dac(&f, 2, false, ALC_PCM);
    mixer(&f, 0x10, 2, 0x11, 0x02);
    mixer(&f, 0x11, 2, 0x10, 0x10);
    selector(&f, 0x12, 1, 0x11);
    pin(&f, 0x0f, PIN_OUT_ONLY, 0x01014010, 1, 0x12);
    hda_plan_outputs(&f.g, &p);
    describe(&f, &p);
    PATH(&p, f.name, 0x0f, 0x12, 0x11, 0x10, 0x02);

    begin(&f, "a pin cannot be a stepping stone", 2, 5);
    dac(&f, 2, false, ALC_PCM);
    pin(&f, 3, PIN_BIDIR, 0x01813050, 1, 2);                    /* line in */
    pin(&f, 4, PIN_OUT_ONLY, 0x01014010, 1, 3);                 /* line out wired to another pin only */
    hda_plan_outputs(&f.g, &p);
    CHECK(p.npins == 0, "pins do not pass signals on");

    begin(&f, "widgets missing from the graph", 2, 6);
    dac(&f, 2, false, ALC_PCM);
    pin(&f, 6, PIN_OUT_ONLY, 0x01014010, 3, 0x30, 4, 2);        /* connection list names nodes that do not exist */
    hda_plan_outputs(&f.g, &p);
    CHECK(p.npins == 1, "dangling connections are skipped");
    PATH(&p, f.name, 6, 2);
}

int main(void) {
    test_qemu();
    test_alc662();
    test_alc255();
    test_shared_selector();
    test_odd_graphs();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
