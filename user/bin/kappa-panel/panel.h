/* Kappa Control Panel: shared declarations. */
#ifndef PANEL_H
#define PANEL_H

#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum { PANE_SYSTEM, PANE_DISPLAY, PANE_INPUT, PANE_DEVICES, PANE_USERS, PANE_POWER, PANE_APPEARANCE, PANE_COUNT };

typedef struct Panel {
    KApp *app;
    int pane;
    bool trace;
    bool dry_run;                       /* --dry-run: power buttons only print what they would do */
    uint64_t now_ms;
    /* appearance */
    bool clock24;
    bool conf_loaded;
    char conf_note[120];
} Panel;
extern Panel P;

void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* util.c */
ssize_t read_file(const char *path, char *buf, size_t cap);
void fmt_bytes(unsigned long long bytes, char *out, size_t cap);
void fmt_kb(unsigned long kb, char *out, size_t cap);
void fmt_duration(unsigned long long ms, char *out, size_t cap);
void draw_heading(KUI *ui, KRect pane, KIcon icon, const char *title, const char *subtitle);
KRect pane_body(KRect pane);            /* the area below the heading */
int  draw_wrapped(KUI *ui, int x, int y, int w, const char *text, uint32_t color);      /* returns the height used */
void kv_line(KUI *ui, int x, int y, int label_w, int w, const char *key, const char *value);

/* conf.c: ~/.config/hakurei.conf */
bool conf_path(char *out, size_t cap);
bool conf_get(const char *key, char *value, size_t cap);
bool conf_set(const char *key, const char *value, char *err, size_t errcap);

/* panes */
void pane_system(KUI *ui, KRect pane);
void pane_display(KUI *ui, KRect pane);
void pane_input(KUI *ui, KRect pane);
void pane_devices(KUI *ui, KRect pane);
void pane_users(KUI *ui, KRect pane);
void pane_power(KUI *ui, KRect pane);
void pane_appearance(KUI *ui, KRect pane);
void devices_refresh(void);

/* doc.c: a scrolling page of headings, key/value lines and small tables */
enum { DOC_HEAD, DOC_KV, DOC_ROW, DOC_HEADROW, DOC_NOTE, DOC_GAP };
#define DOC_COLS 7
typedef struct DocRow { int kind; char text[DOC_COLS][96]; int x[DOC_COLS]; } DocRow;
typedef struct Doc { DocRow *rows; int n, cap, scroll; } Doc;
void doc_clear(Doc *d);
void doc_head(Doc *d, const char *text);
void doc_kv(Doc *d, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void doc_note(Doc *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void doc_gap(Doc *d);
DocRow *doc_row(Doc *d, int kind, const int *xs, int ncols);
void doc_draw(KUI *ui, KRect r, Doc *d);
void doc_dump(const Doc *d);
Doc *devices_doc(void);

#endif
