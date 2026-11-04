/*
 * hda_graph: an HDA codec's audio function group as plain data, and the decisions made from it -- which pins to drive,
 * through which mixers and selectors, from which converters, and which of them to switch on when jacks are plugged in.
 *
 * Pure logic, no hardware and no kernel calls: hda.c fills an HdaGraph by asking the codec; tests/host/hdatest.c fills one
 * from invented Realtek-style node trees and checks the answers on the host (python tests/run_all.py audio_hda_graph).
 *
 * Field meanings follow the Intel HD Audio specification: widget capabilities (parameter 0x09), pin capabilities (0x0C),
 * pin default configuration (verb F1C), amplifier capabilities (0x12 / 0x0D), PCM sizes and rates (0x0A).
 */
#ifndef K_HDA_GRAPH_H
#define K_HDA_GRAPH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum { HDA_WT_OUT = 0, HDA_WT_IN, HDA_WT_MIXER, HDA_WT_SEL, HDA_WT_PIN, HDA_WT_POWER, HDA_WT_KNOB, HDA_WT_BEEP };

#define HDA_WC_STEREO    (1u << 0)
#define HDA_WC_AMP_IN    (1u << 1)
#define HDA_WC_AMP_OUT   (1u << 2)
#define HDA_WC_AMP_OVRD  (1u << 3)          /* the widget has its own amplifier parameters; else the function group's apply */
#define HDA_WC_FMT_OVRD  (1u << 4)          /* the widget has its own PCM parameters; else the function group's apply */
#define HDA_WC_CONN_LIST (1u << 8)
#define HDA_WC_DIGITAL   (1u << 9)
#define HDA_WC_POWER     (1u << 10)
#define HDA_WC_TYPE(c)   (((c) >> 20) & 0xF)

#define HDA_PC_PRESENCE  (1u << 2)          /* pin capabilities */
#define HDA_PC_HP_DRIVE  (1u << 3)
#define HDA_PC_OUT       (1u << 4)
#define HDA_PC_IN        (1u << 5)
#define HDA_PC_EAPD      (1u << 16)

#define HDA_CFG_CONN(c)  ((unsigned)(c) >> 30)          /* pin default configuration: 0 jack, 1 none, 2 fixed, 3 jack and fixed */
#define HDA_CFG_DEV(c)   (((unsigned)(c) >> 20) & 0xF)
#define HDA_DEV_LINE_OUT 0
#define HDA_DEV_SPEAKER  1
#define HDA_DEV_HEADPHONE 2
#define HDA_DEV_OTHER    15

#define HDA_PCM_48K      (1u << 6)          /* parameter 0x0A: rates in bits 11:0 ... */
#define HDA_PCM_16BIT    (1u << 17)         /* ... sample sizes in bits 20:16 */

#define HDA_MAX_CONN 32
#define HDA_MAX_PATH 10
#define HDA_MAX_PINS 12

typedef struct HdaWidget {
    bool     valid;
    uint8_t  type;
    uint32_t caps;
    uint8_t  nconn;
    uint8_t  conn[HDA_MAX_CONN];
    uint32_t pincap, pincfg;
    uint32_t ampout, ampin;                 /* amplifier capabilities, already resolved against the function group's */
    uint32_t pcm;                           /* PCM sizes and rates (converters), already resolved; 0 = unknown */
} HdaWidget;

typedef struct HdaGraph {
    uint8_t    first, count;                /* the widgets' node ids: first .. first + count - 1 */
    HdaWidget *w;                           /* count entries */
} HdaGraph;

typedef struct HdaPath {
    uint8_t n;                              /* nid[0] is the pin, nid[n - 1] the converter */
    uint8_t nid[HDA_MAX_PATH];
    uint8_t idx[HDA_MAX_PATH];              /* the connection index of nid[k] that leads to nid[k + 1] */
} HdaPath;

typedef struct HdaOutPin {
    uint8_t nid;
    uint8_t dev;                            /* HDA_DEV_LINE_OUT, _SPEAKER or _HEADPHONE (HDA_DEV_OTHER for a guessed pin) */
    bool    detect;                         /* a jack that can report whether something is plugged in */
    bool    plugged;                        /* set by the driver from the pin sense */
    bool    want;                           /* set by hda_plan_automute: should this pin be switched on? */
    HdaPath path;
} HdaOutPin;

typedef struct HdaPlan {
    HdaOutPin pin[HDA_MAX_PINS];
    int       npins;
    bool      guessed;                      /* the BIOS declared no output; the single pin here is a guess */
    uint8_t   dac[HDA_MAX_PINS];            /* the distinct converters that carry the stream */
    int       ndacs;
} HdaPlan;

const HdaWidget *hda_widget(const HdaGraph *g, unsigned nid);
const char *hda_dev_name(unsigned dev);

/* Chooses the output pins and their paths. A pin counts when the BIOS declared it a line out, speaker or headphone jack
 * that is not marked "no connection", and it can output. Each gets a path pin <- mixers/selectors <- converter that the
 * stream can use (analog, 16-bit, 48 kHz). Converters are shared only when the graph leaves no choice, and a selector
 * (or a pin with several sources) is never asked to select two different inputs for two pins. */
void hda_plan_outputs(const HdaGraph *g, HdaPlan *plan);

/* From each pin's `plugged` state, decides `want`: headphones silence the speaker and the line out, a plugged line out
 * silences the speaker, pins that cannot sense a jack count as plugged, and if that would leave everything off every pin
 * is switched on (a silent machine because of a broken jack sense is worse than sound on an empty jack). */
void hda_plan_automute(HdaPlan *plan);

#endif
