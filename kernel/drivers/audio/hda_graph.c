/* hda_graph: choosing output pins and converter paths from an HDA codec's widget graph (see hda_graph.h). */
#include "hda_graph.h"
#include <string.h>

const HdaWidget *hda_widget(const HdaGraph *g, unsigned nid) {
    if (!g->w || nid < g->first || nid >= (unsigned)g->first + g->count) return NULL;
    const HdaWidget *w = &g->w[nid - g->first];
    return w->valid ? w : NULL;
}

const char *hda_dev_name(unsigned dev) {
    static const char *const names[] = {
        "line out", "speaker", "headphone", "CD", "S/PDIF out", "digital out", "modem line", "modem handset", "line in", "aux", "mic",
        "telephony", "S/PDIF in", "digital in", "reserved", "other",
    };
    return names[dev & 15];
}

typedef struct Search {
    const HdaGraph *g;
    const uint8_t *avoid;                   /* converters already given to other pins (NULL: no restriction) */
    const uint8_t *sel_used;                /* per node: 1 + the connection index a previous path made it select, or 0 */
    uint8_t visited[256];
} Search;

/* Can this converter carry 48 kHz 16-bit stereo PCM? An unknown capability word is given the benefit of the doubt. */
static bool converter_ok(const HdaWidget *w) {
    if (w->caps & HDA_WC_DIGITAL) return false;                          /* S/PDIF and HDMI converters are not the analog output */
    if (!w->pcm) return true;
    return (w->pcm & HDA_PCM_48K) && (w->pcm & HDA_PCM_16BIT);
}

/* Widgets that pick one of several inputs: selectors, and pins with a connection list. A mixer adds all its inputs instead. */
static bool chooses(const HdaWidget *w) {
    return (w->type == HDA_WT_SEL || w->type == HDA_WT_PIN) && w->nconn > 1;
}

static bool search(Search *s, unsigned nid, int depth, HdaPath *p) {
    const HdaWidget *w = hda_widget(s->g, nid);
    if (!w) return false;
    p->nid[depth] = (uint8_t)nid;
    if (w->type == HDA_WT_OUT) {
        if (!converter_ok(w) || (s->avoid && s->avoid[nid])) return false;
        p->n = (uint8_t)(depth + 1);
        return true;
    }
    if (depth >= HDA_MAX_PATH - 1) return false;
    if (!(w->type == HDA_WT_MIXER || w->type == HDA_WT_SEL || (w->type == HDA_WT_PIN && depth == 0))) return false;
    s->visited[nid] = 1;
    for (unsigned i = 0; i < w->nconn; i++) {
        unsigned c = w->conn[i];
        if (s->visited[c]) continue;
        if (chooses(w) && s->sel_used[nid] && s->sel_used[nid] != i + 1) continue;   /* another pin already needs a different input here */
        p->idx[depth] = (uint8_t)i;
        if (search(s, c, depth + 1, p)) return true;
    }
    s->visited[nid] = 0;
    return false;
}

static bool find_path(const HdaGraph *g, unsigned pin, const uint8_t *avoid, const uint8_t *sel_used, HdaPath *out) {
    Search s;
    memset(&s, 0, sizeof s);
    s.g = g;
    s.avoid = avoid;
    s.sel_used = sel_used;
    memset(out, 0, sizeof *out);
    return search(&s, pin, 0, out);
}

/* Line-out pins of one default association (a 5.1 board's green, black and orange jacks) are one multichannel set. A
 * stereo stream belongs on the first of them, the one with the lowest sequence number; the others would only repeat it. */
static bool group_leader(const HdaGraph *g, unsigned nid, const HdaWidget *w) {
    unsigned assoc = (w->pincfg >> 4) & 0xF, seq = w->pincfg & 0xF;
    if (HDA_CFG_DEV(w->pincfg) != HDA_DEV_LINE_OUT || assoc == 0 || assoc == 15) return true;
    for (unsigned i = 0; i < g->count; i++) {
        unsigned onid = g->first + i;
        const HdaWidget *o = hda_widget(g, onid);
        if (!o || o == w || o->type != HDA_WT_PIN || !(o->pincap & HDA_PC_OUT)) continue;
        if (HDA_CFG_CONN(o->pincfg) == 1 || HDA_CFG_DEV(o->pincfg) != HDA_DEV_LINE_OUT) continue;
        if (((o->pincfg >> 4) & 0xF) != assoc) continue;
        unsigned oseq = o->pincfg & 0xF;
        if (oseq < seq || (oseq == seq && onid < nid)) return false;
    }
    return true;
}

void hda_plan_outputs(const HdaGraph *g, HdaPlan *plan) {
    uint8_t used[256], sel_used[256];
    memset(plan, 0, sizeof *plan);
    memset(used, 0, sizeof used);
    memset(sel_used, 0, sizeof sel_used);
    for (int pass = 0; pass < 2 && !plan->npins; pass++) {   /* pass 0: what the BIOS declared; pass 1: one guess if it declared nothing */
        for (unsigned i = 0; i < g->count && plan->npins < HDA_MAX_PINS; i++) {
            const HdaWidget *w = hda_widget(g, g->first + i);
            unsigned nid = g->first + i;
            if (!w || w->type != HDA_WT_PIN || !(w->pincap & HDA_PC_OUT)) continue;
            unsigned conn = HDA_CFG_CONN(w->pincfg), dev = HDA_CFG_DEV(w->pincfg);
            if (conn == 1) continue;                                          /* "no physical connection" */
            bool declared = dev <= HDA_DEV_HEADPHONE;
            if (pass == 0 && !declared) continue;
            if (pass == 0 && !group_leader(g, nid, w)) continue;
            if (pass == 1 && dev != HDA_DEV_OTHER) continue;
            HdaPath path;
            bool ok = find_path(g, nid, used, sel_used, &path);               /* first choice: a converter nobody else uses */
            if (!ok) ok = find_path(g, nid, NULL, sel_used, &path);           /* otherwise share one */
            if (!ok) continue;
            HdaOutPin *p = &plan->pin[plan->npins++];
            p->nid = (uint8_t)nid;
            p->dev = (uint8_t)(declared ? dev : HDA_DEV_OTHER);
            p->detect = (w->pincap & HDA_PC_PRESENCE) && (conn == 0 || conn == 3);
            p->path = path;
            for (unsigned k = 0; k + 1 < path.n; k++) {
                const HdaWidget *pw = hda_widget(g, path.nid[k]);
                if (pw && chooses(pw)) sel_used[path.nid[k]] = (uint8_t)(path.idx[k] + 1);
            }
            used[path.nid[path.n - 1]] = 1;
            if (pass == 1) plan->guessed = true;
        }
    }
    for (int i = 0; i < plan->npins; i++) {
        uint8_t dac = plan->pin[i].path.nid[plan->pin[i].path.n - 1];
        bool seen = false;
        for (int k = 0; k < plan->ndacs; k++) if (plan->dac[k] == dac) seen = true;
        if (!seen) plan->dac[plan->ndacs++] = dac;
    }
}

void hda_plan_automute(HdaPlan *plan) {
    bool have_detect = false, hp = false, lo = false;
    for (int i = 0; i < plan->npins; i++) {
        const HdaOutPin *p = &plan->pin[i];
        if (!p->detect) continue;
        have_detect = true;
        if (p->dev == HDA_DEV_HEADPHONE && p->plugged) hp = true;
        if (p->dev == HDA_DEV_LINE_OUT && p->plugged) lo = true;
    }
    int on = 0;
    for (int i = 0; i < plan->npins; i++) {
        HdaOutPin *p = &plan->pin[i];
        bool plugged = p->detect ? p->plugged : true;
        if (!have_detect) p->want = true;
        else if (p->dev == HDA_DEV_HEADPHONE) p->want = plugged;
        else if (p->dev == HDA_DEV_LINE_OUT) p->want = plugged && !hp;
        else if (p->dev == HDA_DEV_SPEAKER) p->want = !hp && !lo;
        else p->want = plugged;
        on += p->want;
    }
    if (!on) for (int i = 0; i < plan->npins; i++) plan->pin[i].want = true;
}
