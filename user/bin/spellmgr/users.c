/* The Users tab: accounts from /etc/passwd and groups from /etc/group, with how many spells each account is running. */
#include "spell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Doc g_doc;
static uint64_t g_refreshed_ms;

static int split(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ':');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    return n;
}

void users_refresh(void) {
    doc_clear(&g_doc);
    ProcList procs = { 0 };
    proclist_scan(&procs, NULL);

    doc_head(&g_doc, "Accounts");
    char buf[4096];
    if (read_file("/etc/passwd", buf, sizeof buf) <= 0) doc_note(&g_doc, "The account list (/etc/passwd) cannot be read.");
    else {
        static const int xs[6] = { 0, 120, 180, 240, 400, 560 };
        DocRow *h = doc_row(&g_doc, DOC_HEADROW, xs, 6);
        if (h) {
            snprintf(h->text[0], 96, "Account"); snprintf(h->text[1], 96, "UID"); snprintf(h->text[2], 96, "GID");
            snprintf(h->text[3], 96, "Full name"); snprintf(h->text[4], 96, "Home"); snprintf(h->text[5], 96, "Spells");
        }
        int count = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#' || !line[0]) continue;
            char *f[8];
            if (split(line, f, 8) < 6) continue;
            unsigned uid = (unsigned)strtoul(f[2], NULL, 10);
            int spells = 0;
            for (int i = 0; i < procs.n; i++) if (procs.v[i].uid == uid) spells++;
            DocRow *r = doc_row(&g_doc, DOC_ROW, xs, 6);
            if (!r) break;
            snprintf(r->text[0], 96, "%s%s", f[0], uid == getuid() ? " (you)" : "");
            snprintf(r->text[1], 96, "%s", f[2]);
            snprintf(r->text[2], 96, "%s", f[3]);
            snprintf(r->text[3], 96, "%s", f[4]);
            snprintf(r->text[4], 96, "%s", f[5]);
            snprintf(r->text[5], 96, "%d", spells);
            count++;
        }
        if (!count) doc_note(&g_doc, "There are no accounts in /etc/passwd.");
    }
    doc_gap(&g_doc);

    doc_head(&g_doc, "Groups");
    if (read_file("/etc/group", buf, sizeof buf) <= 0) doc_note(&g_doc, "The group list (/etc/group) cannot be read.");
    else {
        static const int xs[3] = { 0, 120, 180 };
        DocRow *h = doc_row(&g_doc, DOC_HEADROW, xs, 3);
        if (h) { snprintf(h->text[0], 96, "Group"); snprintf(h->text[1], 96, "GID"); snprintf(h->text[2], 96, "Members"); }
        int count = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#' || !line[0]) continue;
            char *f[5];
            int nf = split(line, f, 5);
            if (nf < 3) continue;
            DocRow *r = doc_row(&g_doc, DOC_ROW, xs, 3);
            if (!r) break;
            snprintf(r->text[0], 96, "%s", f[0]);
            snprintf(r->text[1], 96, "%s", f[2]);
            snprintf(r->text[2], 96, "%s", nf > 3 ? f[3] : "");
            count++;
        }
        if (!count) doc_note(&g_doc, "There are no groups in /etc/group.");
    }
    proclist_free(&procs);
    g_refreshed_ms = k_now_ms();
}

Doc *users_doc(void) { return &g_doc; }

void users_tab(KUI *ui, KRect b) {
    if (!g_refreshed_ms || k_now_ms() - g_refreshed_ms > 5000) users_refresh();
    doc_draw(ui, kr_inset(b, 8), &g_doc);
}
