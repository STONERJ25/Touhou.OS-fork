/*
 * web_dump_layout: the box tree as text, for tests and debugging. One line per box: its kind, the element (tag#id.class)
 * and the border box in page coordinates (CSS px, at most two decimals); with WEB_DUMP_TEXT the line boxes and text
 * fragments of block containers, with WEB_DUMP_STYLE a few used values (margins, display).
 */
#include "layout.h"

#include <stdio.h>

static const char *const kind_names[] = { "block", "inline", "text", "replaced", "br", "flex", "grid", "table-wrapper", "table",
                                          "row-group", "row", "cell", "colgroup", "col", "viewport" };

typedef struct D { WebDoc *d; WebWriter w; void *user; int flags; } D;

static void put(D *o, const char *s) { o->w(o->user, s, strlen(s)); }

void web_px_str(WebUnit u, char *out, size_t cap) {
    double v = (double)u / 64.0;
    snprintf(out, cap, "%.2f", v);
    char *dot = strchr(out, '.');
    if (dot) {
        char *e = out + strlen(out) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    if (!strcmp(out, "-0")) strcpy(out, "0");
}

static void desc(D *o, const LBox *b) {
    const WebNode *n = b->node;
    if (b->kind == BK_TEXT) { put(o, "#text"); return; }
    if (!n || n->type != WEB_NODE_ELEMENT) { put(o, "-"); return; }
    put(o, atom_str(&o->d->atoms, n->name));
    if (n->u.el.id) { put(o, "#"); put(o, atom_str(&o->d->atoms, n->u.el.id)); }
    if (n->u.el.nclasses) { put(o, "."); put(o, atom_str(&o->d->atoms, n->u.el.classes[0])); }
}

static void rect(D *o, WebUnit x, WebUnit y, WebUnit w, WebUnit h) {
    char a[32], b[32], c[32], e[32], line[140];
    web_px_str(x, a, sizeof a);
    web_px_str(y, b, sizeof b);
    web_px_str(w, c, sizeof c);
    web_px_str(h, e, sizeof e);
    snprintf(line, sizeof line, " %s,%s %sx%s", a, b, c, e);
    put(o, line);
}

static void indent(D *o, int depth) { for (int i = 0; i < depth && i < 60; i++) put(o, "  "); }

static void dump(D *o, const LBox *b, int depth) {
    if (depth > 400) return;
    WebUnit x, y;
    box_abs(b, &x, &y);
    indent(o, depth);
    put(o, b->kind < sizeof kind_names / sizeof kind_names[0] ? kind_names[b->kind] : "?");
    if (b->flags & BF_ANON) put(o, " anon");
    if (b->flags & BF_FLOAT) put(o, " float");
    if (b->flags & BF_ABS) put(o, " abs");
    put(o, " ");
    desc(o, b);
    if (b->kind != BK_INLINE && b->kind != BK_TEXT && b->kind != BK_BR) rect(o, x, y, b->w, b->h);
    if (b->kind == BK_TEXT && (o->flags & WEB_DUMP_TEXT)) {
        put(o, " \"");
        o->w(o->user, b->text, b->tlen > 60 ? 60 : b->tlen);
        put(o, "\"");
    }
    if (o->flags & WEB_DUMP_STYLE && b->kind != BK_TEXT) {
        char m[32], line[160];
        put(o, " m=");
        for (int i = 0; i < 4; i++) { web_px_str(b->m[i], m, sizeof m); snprintf(line, sizeof line, "%s%s", i ? "," : "", m); put(o, line); }
    }
    put(o, "\n");
    if ((o->flags & WEB_DUMP_TEXT) && b->nlines) {
        for (uint32_t i = 0; i < b->nlines; i++) {
            const Line *ln = &b->lines[i];
            indent(o, depth + 1);
            put(o, "line");
            rect(o, x + ln->left, y + ln->y, ln->width, ln->h);
            char base[32], line[64];
            web_px_str(y + ln->baseline, base, sizeof base);
            snprintf(line, sizeof line, " baseline=%s\n", base);
            put(o, line);
            for (uint32_t k = ln->first; k < ln->first + ln->n && k < b->nfrags; k++) {
                const Frag *f = &b->frags[k];
                indent(o, depth + 2);
                if (f->kind == FR_TEXT) {
                    put(o, "text \"");
                    o->w(o->user, f->box->text + f->start, f->len);
                    put(o, "\"");
                } else if (f->kind == FR_BOX) { put(o, "box "); desc(o, f->box); }
                else if (f->kind == FR_ATOMIC) { put(o, "atomic "); desc(o, f->box); }
                else put(o, "marker");
                rect(o, x + f->x, y + f->y, f->w, f->h);
                put(o, "\n");
            }
        }
    }
    for (const LBox *c = b->first; c; c = c->next) dump(o, c, depth + 1);
}

void web_dump_layout(WebDoc *d, WebWriter w, void *user, int flags) {
    if (d->style_dirty || d->layout_dirty || !d->lay) web_doc_layout(d, d->vw, d->vh);
    LayoutState *st = d->lay;
    if (!st || !st->root) return;
    D o = { d, w, user, flags };
    dump(&o, st->root, 0);
}
