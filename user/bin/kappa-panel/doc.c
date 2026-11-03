/* A scrolling page of headings, key/value lines and small tables: the Hardware and Users tabs are built from it. */
#include "panel.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void doc_clear(Doc *d) { d->n = 0; }

void doc_free(Doc *d) {
    free(d->rows);
    memset(d, 0, sizeof *d);
}

static DocRow *doc_add(Doc *d, int kind) {
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 64;
        DocRow *nr = realloc(d->rows, (size_t)nc * sizeof *nr);
        if (!nr) return NULL;
        d->rows = nr;
        d->cap = nc;
    }
    DocRow *r = &d->rows[d->n++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    return r;
}

void doc_head(Doc *d, const char *text) {
    DocRow *r = doc_add(d, DOC_HEAD);
    if (r) snprintf(r->text[0], sizeof r->text[0], "%s", text);
}

void doc_kv(Doc *d, const char *key, const char *fmt, ...) {
    DocRow *r = doc_add(d, DOC_KV);
    if (!r) return;
    snprintf(r->text[0], sizeof r->text[0], "%s", key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text[1], sizeof r->text[1], fmt, ap);
    va_end(ap);
}

void doc_note(Doc *d, const char *fmt, ...) {
    DocRow *r = doc_add(d, DOC_NOTE);
    if (!r) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text[0], sizeof r->text[0], fmt, ap);
    va_end(ap);
}

void doc_gap(Doc *d) { doc_add(d, DOC_GAP); }

DocRow *doc_row(Doc *d, int kind, const int *xs, int ncols) {
    DocRow *r = doc_add(d, kind);
    if (!r) return NULL;
    if (ncols > DOC_COLS - 1) ncols = DOC_COLS - 1;
    for (int i = 0; i < ncols; i++) r->x[i] = xs[i];
    r->x[ncols] = r->x[ncols - 1] + 1000;                              /* the last column runs to the edge */
    return r;
}

static int row_cols(const DocRow *r);

void doc_dump(const Doc *d) {
    for (int i = 0; i < d->n; i++) {
        const DocRow *r = &d->rows[i];
        switch (r->kind) {
        case DOC_HEAD: printf("== %s ==\n", r->text[0]); break;
        case DOC_KV: printf("%s: %s\n", r->text[0], r->text[1]); break;
        case DOC_NOTE: printf("%s\n", r->text[0]); break;
        case DOC_GAP: break;
        default:
            for (int c = 0, n = row_cols(r); c < n; c++) printf("%s%s", c ? " | " : "", r->text[c]);
            putchar('\n');
            break;
        }
    }
}

static int row_cols(const DocRow *r) {                                    /* the sentinel offset marks the last column */
    for (int c = 0; c < DOC_COLS - 1; c++) if (r->x[c + 1] - r->x[c] >= 1000) return c + 1;
    return DOC_COLS - 1;
}

static int row_height(const DocRow *r) {
    switch (r->kind) {
    case DOC_HEAD: return 30;
    case DOC_GAP: return 8;
    default: return 18;
    }
}

void doc_draw(KUI *ui, KRect r, Doc *d) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, KB_SUNKEN, ui->theme->field_bg);
    KRect in = kr_inset(r, 2);
    int total = 0;
    for (int i = 0; i < d->n; i++) total += row_height(&d->rows[i]);
    total += 6;
    bool bar = total > in.h;
    KRect body = bar ? kr(in.x, in.y, in.w - 14, in.h) : in;
    if (k_hover(ui, r) && ui->wheel) d->scroll -= ui->wheel * 36;
    int maxs = total > body.h ? total - body.h : 0;
    if (d->scroll > maxs) d->scroll = maxs;
    if (d->scroll < 0) d->scroll = 0;

    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, body));
    int key_w = 0;                                                     /* the key column is as wide as the longest key */
    for (int i = 0; i < d->n; i++)
        if (d->rows[i].kind == DOC_KV) { int w = (int)strlen(d->rows[i].text[0]) * 8 + 24; if (w > key_w) key_w = w; }
    if (key_w < 120) key_w = 120;
    if (key_w > body.w / 2) key_w = body.w / 2;
    int y = body.y + 4 - d->scroll;
    for (int i = 0; i < d->n; i++) {
        const DocRow *row = &d->rows[i];
        int h = row_height(row);
        if (y + h < body.y || y > body.y + body.h) { y += h; continue; }
        int x0 = body.x + 12;
        switch (row->kind) {
        case DOC_HEAD:
            kc_text_bold(cv, x0, y + 8, row->text[0], ui->theme->accent_dark);
            kc_hline(cv, body.x + 8, y + 27, body.w - 16, kc_shade(ui->theme->field_bg, 80));
            break;
        case DOC_KV:
            kc_text_in(cv, kr(x0, y + 1, key_w - 8, 16), row->text[0], ui->theme->text_dim, KA_LEFT);
            kc_text_in(cv, kr(x0 + key_w, y + 1, body.x + body.w - x0 - key_w - 10, 16), row->text[1], ui->theme->text, KA_LEFT);
            break;
        case DOC_HEADROW:
        case DOC_ROW:
            if (row->kind == DOC_HEADROW) kc_fill(cv, kr(body.x + 8, y, body.w - 16, 18), kc_shade(ui->theme->window_bg, 96));
            else if ((i & 1) == 0) kc_fill(cv, kr(body.x + 8, y, body.w - 16, 18), kc_shade(ui->theme->field_bg, 97));
            for (int c = 0, ncols = row_cols(row); c < ncols; c++) {
                int w = row->x[c + 1] - row->x[c] - 6;
                int maxw = body.x + body.w - (x0 + row->x[c]) - 8;
                if (w > maxw) w = maxw;
                if (w < 8) break;
                const char *t = row->text[c];
                if (row->kind == DOC_HEADROW) {
                    kc_set_clip(cv, kr_intersect(old, kr_intersect(body, kr(x0 + row->x[c], y, w, 18))));
                    kc_text_bold(cv, x0 + row->x[c], y + 1, t, ui->theme->text);
                    kc_set_clip(cv, kr_intersect(old, body));
                } else kc_text_in(cv, kr(x0 + row->x[c], y + 1, w, 16), t, ui->theme->text, KA_LEFT);
            }
            break;
        case DOC_NOTE:
            kc_text_in(cv, kr(x0, y + 1, body.w - 24, 16), row->text[0], ui->theme->text_dim, KA_LEFT);
            break;
        default: break;
        }
        y += h;
    }
    cv->clip = old;
    if (bar) k_scrollbar(ui, kr(in.x + in.w - 14, in.y, 14, in.h), &d->scroll, total, body.h);
}
